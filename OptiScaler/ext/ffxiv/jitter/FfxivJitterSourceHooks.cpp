#include "pch.h"

#include "ext/ffxiv/jitter/FfxivJitterSourceHooks.h"

#include "ext/core/ProcessMemory.h"
#include "ext/ffxiv/GameBuild.h"
#include "ext/ffxiv/jitter/FfxivJitterSourceAnchors.h"
#include "ext/ffxiv/jitter/FfxivJitterSourceDetours.h"
#include "ext/ffxiv/quality/NativeQualityRetry.h"
#include "ext/ffxiv/quality/NativeQualityTransaction.h"

#include <intrin.h>

#include <array>
#include <atomic>

namespace ext::ffxiv::jitter::source::hooks
{
namespace
{
namespace retry = ext::ffxiv::quality::retry;
namespace transaction = ext::ffxiv::quality::transaction;

using Generator = std::uintptr_t (*)(std::uintptr_t, float*, float*, unsigned, unsigned);
using Getter = void* (*)(std::uintptr_t);

// The trampolines. The transaction sets each to the raw function address immediately before DetourAttach on every attempt (Detours then points it
// at the trampoline), so an attempt that failed can never leave one stale.
Generator g_generator = nullptr;
Getter g_getter = nullptr;
std::uintptr_t g_base = 0;                     // the executable's base; written before the first attach, read-only afterwards
const JitterSourceBuild* g_build = nullptr;    // same
std::atomic<bool> g_guardFailed { false };

bool ReadBytes(std::uintptr_t address, void* out, std::size_t bytes) { return ext::core::ReadProcessBytes(address, out, bytes); }

template <class T> bool ReadValue(std::uintptr_t address, T& out) { return ReadBytes(address, &out, sizeof(out)); }

// Everything the generator plan needs, read from the game. false = a read failed (the call is left alone).
bool ReadInputs(const JitterSourceBuild& b, std::uint32_t phases, GeneratorInputs& in)
{
    std::uintptr_t settings = 0, renderer = 0, device = 0;
    if (!ReadValue(g_base + b.settingsPointerRva, settings) || !ReadValue(g_base + b.rendererPointerRva, renderer) ||
        !ReadValue(g_base + b.devicePointerRva, device) || settings == 0 || renderer == 0 || device == 0)
        return false;

    // settings: scale at +0x4c, mode byte at +0x54, amount at +0x78, read as one block starting at the scale
    constexpr std::uint32_t kBlock = 0x30;
    const std::uint32_t modeAt = b.settingsModeOffset - b.settingsScaleOffset;
    const std::uint32_t amountAt = b.settingsAmountOffset - b.settingsScaleOffset;
    if (modeAt + 1 > kBlock || amountAt + sizeof(float) > kBlock)
        return false;
    std::array<unsigned char, kBlock> block {};
    if (!ReadBytes(settings + b.settingsScaleOffset, block.data(), block.size()))
        return false;
    std::memcpy(&in.scale, block.data(), sizeof(float));
    std::memcpy(&in.amount, block.data() + amountAt, sizeof(float));
    in.dlssMode = block[modeAt] == 2;

    std::uint16_t extent[3] {};
    std::uint32_t render[2] {}, output[2] {};
    if (!ReadBytes(renderer + b.rendererExtentOffset, extent, sizeof(extent)) ||
        !ReadBytes(renderer + b.rendererSceneSizeOffset, render, sizeof(render)) ||
        !ReadBytes(device + b.deviceOutputSizeOffset, output, sizeof(output)))
        return false;
    in.extent0 = extent[0];
    in.extent2 = extent[2];
    in.renderW = render[0];
    in.renderH = render[1];
    in.outW = output[0];
    in.outH = output[1];
    in.phases = phases;
    return true;
}

// The detour of the jitter generator. The return address is read FIRST (before anything that could change the frame) and handed to the body in
// FfxivJitterSourceDetours.h (RunGenerate), which touches only the camera update's call for the world camera's jitter object.
std::uintptr_t Generate(std::uintptr_t opaque, float* x, float* y, unsigned frame, unsigned phases)
{
    GenerateArgs args;
    args.caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    args.expectedCaller = g_build != nullptr ? g_base + g_build->generatorCallReturnRva : 0;
    args.opaque = opaque;
    args.x = x;
    args.y = y;
    args.frame = frame;
    args.phases = phases;
    return RunGenerate(
        State(), args, [](std::uintptr_t o, float* gx, float* gy, unsigned f, unsigned p) { return g_generator(o, gx, gy, f, p); },
        [](std::uint32_t p, GeneratorInputs& in) { return g_build != nullptr && ReadInputs(*g_build, p, in); }, [] { return GetTickCount64(); });
}

// The detour of the camera getter: observe only (RunObserve). The world-scene NGX packet producer is the one caller sampled (its return address is
// exe+0x374122); the pointer it gets is the game's own, unchanged.
void* Observe(std::uintptr_t camera)
{
    ObserveArgs args;
    args.caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    args.expectedCaller = g_build != nullptr ? g_base + g_build->producerReturnRva : 0;
    args.camera = camera;
    return RunObserve(
        State(), args, [](std::uintptr_t c) { return g_getter(c); },
        [](const void* object, float* out)
        { return ReadBytes(reinterpret_cast<std::uintptr_t>(object), out, 4 * sizeof(float)); },
        [](std::uint32_t* size)
        {
            std::uintptr_t renderer = 0;
            return g_build != nullptr && ReadValue(g_base + g_build->rendererPointerRva, renderer) && renderer != 0 &&
                   ReadBytes(renderer + g_build->rendererSceneSizeOffset, size, 2 * sizeof(std::uint32_t));
        },
        [] { return GetTickCount64(); });
}

const GameBuild* Matched()
{
    static const GameBuild* build = MatchCurrentProcess();
    return build;
}

// One install attempt: the identity and the instruction guards (deterministic for an exe: a refusal is Permanent and never retried), then ONE
// transaction with both hooks (all or nothing).
retry::StepResult Attempt()
{
    constexpr retry::StepResult kRefused { retry::Outcome::Permanent, retry::Stage::Guard, 0, 0 };
    const GameBuild* build = Matched();
    if (build == nullptr)
    {
        g_guardFailed.store(false);
        LOG_INFO("Jitter source: the game executable is not the supported build; the hooks are not installed");
        return kRefused;
    }
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const AnchorResult guards = VerifyAnchors(
        [](std::uintptr_t address, void* out, std::size_t bytes) { return ReadBytes(address, out, bytes); }, build->jitterSource, base);
    if (!guards.ok)
    {
        g_guardFailed.store(true);
        LOG_WARN("Jitter source: instruction guard '{}' does not match this executable; the hooks are not installed", guards.failed);
        return kRefused;
    }

    g_base = base;
    g_build = &build->jitterSource;
    const std::array<transaction::HookSpec, 2> specs { {
        { reinterpret_cast<void**>(&g_generator), reinterpret_cast<void*>(base + build->jitterSource.generatorRva),
          reinterpret_cast<void*>(&Generate), retry::Stage::AttachJitterGenerator },
        { reinterpret_cast<void**>(&g_getter), reinterpret_cast<void*>(base + build->jitterSource.cameraGetterRva),
          reinterpret_cast<void*>(&Observe), retry::Stage::AttachCameraGetter },
    } };
    const retry::StepResult result = transaction::AttemptHooks(specs.data(), specs.size());
    if (result.outcome == retry::Outcome::Installed)
        LOG_INFO("Jitter source: hooks installed (generator exe+{:#x}, camera getter exe+{:#x}, transaction result {})", build->jitterSource.generatorRva,
                 build->jitterSource.cameraGetterRva, result.error);
    return result;
}

struct InstallLog
{
    void Failed(int attempt, int maxAttempts, const retry::StepResult& failure) const
    {
        LOG_WARN("Jitter source: install attempt {}/{} failed at {}, error {}, thread {}; retrying", attempt, maxAttempts,
                 retry::StageName(failure.stage), failure.error, failure.threadId);
    }
    void GaveUp(int attempt, int maxAttempts, const retry::StepResult& failure) const
    {
        LOG_WARN("Jitter source: install attempt {}/{} failed at {}, error {}, thread {}; giving up; the fix stays off for this session", attempt,
                 maxAttempts, retry::StageName(failure.stage), failure.error, failure.threadId);
    }
    void Recovered(int attempt) const { LOG_INFO("Jitter source: hooks installed on attempt {}", attempt); }
};

struct AttemptStep
{
    retry::StepResult operator()() const { return Attempt(); }
};

struct NowMs
{
    std::uint64_t operator()() const { return GetTickCount64(); }
};

using Installer = retry::RetryingInstaller<AttemptStep, NowMs, InstallLog>;

// A function-local static, not a namespace-scope variable: the project builds with _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR, so the installer (it
// holds a std::mutex) is not constant-initialised, and this form cannot be reached before it is built.
Installer& TheInstaller()
{
    static Installer installer { AttemptStep {}, NowMs {}, InstallLog {} };
    return installer;
}
} // namespace

SharedState& State()
{
    static SharedState state;
    return state;
}

HooksView View()
{
    HooksView view;
    view.buildMatched = Matched() != nullptr;
    view.installed = TheInstaller().Ensure(); // one atomic load once the outcome is final
    view.guardsOk = !g_guardFailed.load();
    return view;
}
} // namespace ext::ffxiv::jitter::source::hooks
