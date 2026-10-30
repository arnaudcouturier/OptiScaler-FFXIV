#pragma once
// Native FFXIV scene-resolution hooks behind INativeQuality (C1, C5; docs/SEAMS-catalog.md "upscaler",
// docs/SEAMS.md 3.8). Detours two native functions so a forced preset reaches the game's own per-quality render
// table, and reads the native settings struct for the native-DLAA gate. Ported from the oracle's
// FfxivNativeQuality (misc/FfxivNativeQuality.h, folded together with its install logic into
// upscalers/IFeature.cpp there; split back out into its own game-module directory here, see SEAMS.md 3.8). Every
// RVA/offset/byte guard comes from ext::ffxiv::GameBuild.h; a game patch means editing that file, not this one.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>

#include "ext/core/ExtConfig.h"
#include "ext/core/ProcessMemory.h"
#include "ext/ffxiv/ActiveBuild.h"
#include "ext/ffxiv/BuildBounds.h"
#include "ext/ffxiv/GameBuild.h"
#include "ext/ffxiv/quality/NativeQualityRetry.h"
#include "ext/ffxiv/quality/NativeQualityTransaction.h"
#include "ext/upscaler/UpscalerSeams.h"

namespace ext::ffxiv::quality::FfxivNativeQualityHooks
{
// Canonical implementation lives in ext::core::ReadProcessBytes (ext/core/ProcessMemory.h);
// kept as a named forwarder since this module calls
// ReadMemory(const void*, ...) throughout.

inline bool ReadMemory(const void* address, void* output, size_t bytes)
{
    return ext::core::ReadProcessBytes(reinterpret_cast<uintptr_t>(address), output, bytes);
}

inline uintptr_t Base() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }

// The build this module reads is ext::ffxiv::ActiveBuild() (the resolved build, which equals GameBuild.h on the known build; the compile-time table
// under [FFXIV] FfxivLegacyGameBuild), and only while the build gate (ext/ffxiv/resolve/Gate.h) has enabled the feature that asks. Nothing is
// cached: the gate is immutable once the startup has published it, and a refusal computed BEFORE the startup (ActiveStarted() false) must never
// stick, so the answers below are recomputed on every call (two atomic loads).
inline const NativeQualityBuild* BuildFor(ext::ffxiv::resolve::Feature feature)
{
    const GameBuild* build = ext::ffxiv::BuildFor(feature); // the one shared accessor (ActiveBuild.h): the build only while the gate enabled THIS feature
    return build != nullptr ? &build->nativeQuality : nullptr;
}
// The DLAA gate (reads the settings struct, hooks nothing).
inline const NativeQualityBuild* Build() { return BuildFor(ext::ffxiv::resolve::Feature::QualityRead); }
// The two detours, the direct resize call and the display-size cache write.
inline const NativeQualityBuild* HookBuild() { return BuildFor(ext::ffxiv::resolve::Feature::QualityHooks); }

// ---- Native DLAA gate (FfxivNativeQuality::NativeDlaaRequired in the oracle) --------------------------------------

// The snapshot buffer is a fixed maximum (ext/ffxiv/BuildBounds.h kMaxSettingsSnapshotBytes); the bytes read are max(the resolved settings offsets) + 1,
// which is exactly the old compile-time 0x56 on the known build, and a healed offset at or above the maximum is refused here and by the gate.

// Byte-verifies the settings read path once (fail closed, log once) and caches the result: both Attempt and
// DlaaRequiredNow gate on this, matching the oracle's EnsureFfxivNativeQuality probe.
inline bool SettingsGuardOk()
{
    if (!ext::ffxiv::ActiveStarted())
        return false; // not decided yet: refuse for now, never latch it (the static below is only reached after the startup)
    static const bool ok = []
    {
        const auto* build = Build();
        if (build == nullptr)
            return false;

        auto actual = build->settingsInstructions;
        const bool match =
            ReadMemory(reinterpret_cast<const void*>(Base() + build->settingsInstructionsRva), actual.data(),
                       actual.size()) &&
            actual == build->settingsInstructions;

        if (!match)
            LOG_WARN("FFXIV probe: unsupported executable instructions; settings read disabled");

        return match;
    }();
    return ok;
}

inline bool DlaaRequiredNow()
{
    if (!SettingsGuardOk())
        return false;

    const auto* build = Build();
    if (build == nullptr || !ext::ffxiv::SettingsOffsetsInBounds(*build))
        return false;
    const auto base = Base();

    // sized from the resolved offsets (0x56 on the known build); every index below is < size because size = max(offset) + 1
    const size_t size = ext::ffxiv::SettingsSnapshotBytes(*build);
    uintptr_t settings = 0;
    std::array<unsigned char, ext::ffxiv::kMaxSettingsSnapshotBytes> snapshot {};
    const bool valid =
        size <= snapshot.size() &&
        ReadMemory(reinterpret_cast<const void*>(base + build->settingsPointerRva), &settings, sizeof(settings)) &&
        settings != 0 && ReadMemory(reinterpret_cast<const void*>(settings), snapshot.data(), size);
    const bool required = valid && ext::ffxiv::DlaaRequiredFromSnapshot(snapshot.data(), size, *build);

    // Log only when the gate flips (first call included); the exchange keeps the per-frame path cheap.
    static std::atomic<int> lastRequired { -1 };
    if (lastRequired.exchange(required ? 1 : 0) != (required ? 1 : 0))
        LOG_INFO("FFXIV native DLAA gate: settings={:#x} read={} +0x44={:#04x} +0x45={:#04x} +0x54={:#04x} "
                 "+0x55={:#04x} required={}",
                 settings, valid, snapshot[build->settingsQualityOffset], snapshot[build->settingsDlaaOffset],
                 snapshot[build->settingsModeOffset], snapshot[build->settingsSelectorOffset], required);
    return required;
}

inline std::atomic<uint64_t> dlaaCacheTick { 0 };
inline std::atomic<int> dlaaCacheValue { 0 };
inline std::atomic<bool> dlaaCacheValid { false };

// DlaaRequired, cached per frame (ext::game::INativeQuality): re-reads the settings struct at most once per
// distinct GetTickCount64() value instead of on every call site (EffectivePerfQuality, ApplyForcedQuality and the
// renderer-update detour below all ask, at least once a frame each). No mutex, no string compare: three relaxed
// atomics on the cache-hit path.
inline bool DlaaRequiredCached()
{
    const auto tick = GetTickCount64();
    if (dlaaCacheValid.load(std::memory_order_relaxed) && dlaaCacheTick.load(std::memory_order_relaxed) == tick)
        return dlaaCacheValue.load(std::memory_order_relaxed) != 0;

    const bool required = DlaaRequiredNow();
    dlaaCacheValue.store(required ? 1 : 0, std::memory_order_relaxed);
    dlaaCacheTick.store(tick, std::memory_order_relaxed);
    dlaaCacheValid.store(true, std::memory_order_relaxed);
    return required;
}

// ---- Install / detours (InstallFfxivNativeQuality, ObserveFfxivQuery, ObserveFfxivRendererUpdate in the oracle) --

inline std::atomic<bool> available { false };
inline std::atomic<int> requestedQuality { -1 };
inline std::atomic<unsigned int> requestSequence { 0 };

using RendererUpdate = void (*)(uintptr_t, float);
using ResizeCallbacks = unsigned char (*)(uintptr_t);
using QuerySize = uintptr_t (*)(const unsigned int*, unsigned int*, unsigned int*, unsigned int*);

inline RendererUpdate originalRendererUpdate = nullptr;
inline ResizeCallbacks dispatchResizeCallbacks = nullptr;
inline QuerySize originalQuerySize = nullptr;

inline void Request(int quality)
{
    requestedQuality.store(quality >= 0 && quality <= 5 ? quality : -1);
    requestSequence.fetch_add(1);
}

// Renderer-update detour body: a pending Request() reaches the native per-quality table by invalidating its
// display-size cache and calling dispatchResizeCallbacks directly (not detoured), which makes the native renderer
// re-query NGX. Runs once per sequence number, on the game's own update thread.
inline void ProcessPendingRequest(uintptr_t renderer)
{
    if (DlaaRequiredCached())
        return; // native DLAA already owns the scene size; do not fight it with a preset request

    static unsigned int handled = 0;
    const auto sequence = requestSequence.load();
    if (sequence == handled)
        return;

    const auto* build = HookBuild();
    if (build == nullptr)
        return;
    const auto base = Base();

    uintptr_t settings = 0, manager = 0, context = 0, device = 0, callbacks = 0;
    unsigned char type = 0, flags = 0;
    if (!ReadMemory(reinterpret_cast<void*>(base + build->settingsPointerRva), &settings, sizeof(settings)) ||
        settings == 0 ||
        !ReadMemory(reinterpret_cast<void*>(settings + build->settingsModeOffset), &type, sizeof(type)) ||
        type != 2 ||
        !ReadMemory(reinterpret_cast<void*>(base + build->managerPointerRva), &manager, sizeof(manager)) ||
        manager == 0 ||
        !ReadMemory(reinterpret_cast<void*>(manager + build->managerContextOffset), &context, sizeof(context)) ||
        context == 0 ||
        !ReadMemory(reinterpret_cast<void*>(context + build->contextFlagsOffset), &flags, sizeof(flags)) ||
        (flags & 1) == 0 || (flags & 0x30) != 0 ||
        !ReadMemory(reinterpret_cast<void*>(base + build->devicePointerRva), &device, sizeof(device)) ||
        device == 0 ||
        !ReadMemory(reinterpret_cast<void*>(device + build->deviceCallbacksOffset), &callbacks, sizeof(callbacks)) ||
        callbacks == 0)
        return;

    // Invalidate only the quality table's display-size cache. The native callback then queries NGX again and
    // propagates the result to rendering textures on the game's update thread.
    const unsigned long long invalidDisplay = 0;
    SIZE_T written = 0;
    if (!WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(context + build->contextCacheOffset),
                             &invalidDisplay, sizeof(invalidDisplay), &written) ||
        written != sizeof(invalidDisplay))
        return;

    const auto result = dispatchResizeCallbacks(callbacks);
    handled = sequence;
    unsigned int size[2] {};
    ReadMemory(reinterpret_cast<void*>(renderer + build->rendererSceneSizeOffset), size, sizeof(size));
    LOG_INFO("FFXIV native quality: request={} preset={} callbacks={} scene={}x{} thread={}", sequence,
             requestedQuality.load(), result, size[0], size[1], GetCurrentThreadId());
}

inline void ObserveRendererUpdate(uintptr_t renderer, float elapsed)
{
    ProcessPendingRequest(renderer);
    originalRendererUpdate(renderer, elapsed);
}

// Query-size detour body: once a pending request's resize round-trip lands back here with a matching answer, also
// constrain the native scene selector (not just NGX feature-creation metadata). Exact equality on render dims --
// a different rule from ApplyForcedQuality's 32px slack (ext/upscaler/ForcedQuality.h): this compares the native
// query's own target against the fresh LastQualityAnswer it was itself responsible for producing, not a possibly
// stale earlier create.
inline uintptr_t ObserveQuery(const unsigned int* display, unsigned int* optimal, unsigned int* minimum,
                               unsigned int* maximum)
{
    const auto result = originalQuerySize(display, optimal, minimum, maximum);
    const int requested = requestedQuality.load();
    if ((result & 0xff) != 0 && requested >= 0 && requested <= 5)
    {
        const auto answer = ext::upscaler::LastQualityAnswer();
        unsigned int target[2] {}, output[2] {};
        if (ReadMemory(optimal, target, sizeof(target)) && ReadMemory(display, output, sizeof(output)) &&
            answer.quality == requested && answer.renderWidth == target[0] && answer.renderHeight == target[1] &&
            target[0] > 0 && target[1] > 0 && target[0] <= output[0] && target[1] <= output[1])
        {
            memcpy(minimum, target, sizeof(target));
            memcpy(maximum, target, sizeof(target));
        }
    }
    return result;
}

// The thread enlistment and the hook transaction this note is about live in NativeQualityTransaction.h; the note applies
// unchanged to every install attempt, retries included (at most three per session, so a retry repeats the exposure at most
// twice more).
// Known, accepted risk (reviewed, not an oversight): THREAD_SUSPEND_RESUME (requested on OpenThread in SnapshotLiveThreads) is
// only an access right -- it does not suspend anything by itself. The actual suspension happens inside the DetourUpdateThread
// calls, each of which allocates its own per-thread record while earlier threads in the same call are already suspended;
// DetourAttach allocates one record per hook too, all before the transaction resumes anyone. Verified in the vendored
// detours.lib (disassembly): those allocations are the CRT's operator new (a 16-byte thread record, a 48-byte operation
// record), so the hazard is real, not hypothetical: a suspended thread that holds the process heap lock would block the next
// allocation. Trampolines are VirtualAlloc'd, a different path that does not mitigate it; DetourTransactionCommit and Abort
// allocate nothing (they only free with operator delete). Deliberately not restructured: the allocations happen inside Detours
// and the window lasts a few milliseconds (the game log shows the whole first install in 58 ms). The same note applies to
// ext::core::DetourThreadScope (ext/core/DetourThreads.h), which shares this enlistment pattern with the two other call
// site that patches D3D11 vtable slots (ext::ffxiv::lighting::FfxivLightingCapture).// One install attempt. The guards below are deterministic for a given exe, so a refusal there is Permanent (never retried)
// and keeps the one warning it always logged. The Detours half (thread snapshot + one hook transaction) is
// transaction::AttemptHooks (NativeQualityTransaction.h), whose failures it classifies as transient or permanent. Nothing
// is logged between a transaction's begin and end; the retry logging happens in InstallLog, after the threads run again.
inline retry::StepResult Attempt()
{
    using retry::Outcome;
    using retry::Stage;
    constexpr retry::StepResult kRefused { Outcome::Permanent, Stage::Guard, 0, 0 };

    // Before the startup published the gate: not a verdict. Transient, so the retry installer asks again (250 ms, then 2 s) instead of latching.
    if (!ext::ffxiv::ActiveStarted())
        return { Outcome::Transient, Stage::Guard, 0, 0 };

    // The build gate (Gate.h): the exe, the seams of the whole group and the unknown-build key decided at the startup (its [ffxiv-gate] line says why
    // when it is off). Under the rollback key this is "the exact known build", the old MatchCurrentProcess.
    const auto* build = HookBuild();
    if (build == nullptr)
        return kRefused;

    if (!SettingsGuardOk())
        return kRefused; // same probe NativeDlaaRequired fails closed on; EnsureFfxivNativeQuality refused to install here too

    const auto base = Base();

    auto queryActual = build->querySizeInstructions;
    if (!ReadMemory(reinterpret_cast<void*>(base + build->querySizeRva), queryActual.data(), queryActual.size()) ||
        queryActual != build->querySizeInstructions)
    {
        LOG_WARN("FFXIV native quality: instruction mismatch; hooks disabled");
        return kRefused;
    }

    auto updateActual = build->rendererUpdateInstructions;
    auto dispatchActual = build->resizeCallbacksInstructions;
    if (!ReadMemory(reinterpret_cast<void*>(base + build->rendererUpdateRva), updateActual.data(),
                     updateActual.size()) ||
        !ReadMemory(reinterpret_cast<void*>(base + build->resizeCallbacksRva), dispatchActual.data(),
                     dispatchActual.size()) ||
        updateActual != build->rendererUpdateInstructions || dispatchActual != build->resizeCallbacksInstructions)
    {
        LOG_WARN("FFXIV native quality: update instruction mismatch; hooks disabled");
        return kRefused;
    }

    dispatchResizeCallbacks = reinterpret_cast<ResizeCallbacks>(base + build->resizeCallbacksRva);

    // originalQuerySize / originalRendererUpdate are set to the raw function addresses by the transaction itself, just before
    // each DetourAttach, on every attempt: a retry never starts from whatever an earlier attempt left in them.
    const std::array<transaction::HookSpec, 2> specs { {
        { reinterpret_cast<void**>(&originalQuerySize), reinterpret_cast<void*>(base + build->querySizeRva),
          reinterpret_cast<void*>(&ObserveQuery), Stage::AttachQuerySize },
        { reinterpret_cast<void**>(&originalRendererUpdate), reinterpret_cast<void*>(base + build->rendererUpdateRva),
          reinterpret_cast<void*>(&ObserveRendererUpdate), Stage::AttachRendererUpdate },
    } };
    const retry::StepResult result = transaction::AttemptHooks(specs.data(), specs.size());
    if (result.outcome != Outcome::Installed)
        return result;

    available.store(true);
    const int forced = ext::Cfg().upscaler.forcePerfQuality.value_or_default();
    if (forced >= 0 && forced <= 5)
        Request(forced);
    LOG_INFO("FFXIV native quality v2: hook transaction result={} (0=installed); native scene updates enabled",
             result.error);
    return result;
}

// The retry's log lines, at most four per session: one per failed attempt that will be retried, one when it gives up
// (carrying the details of the last failure), one when a later attempt installs. A refusal by the guards above is not
// repeated here (it logged its own warning).
struct InstallLog
{
    void Failed(int attempt, int maxAttempts, const retry::StepResult& failure) const
    {
        LOG_WARN("FFXIV native quality: install attempt {}/{} failed at {}, error {}, thread {}; retrying", attempt,
                 maxAttempts, retry::StageName(failure.stage), failure.error, failure.threadId);
    }
    void GaveUp(int attempt, int maxAttempts, const retry::StepResult& failure) const
    {
        LOG_WARN("FFXIV native quality: install attempt {}/{} failed at {}, error {}, thread {}; giving up; native "
                 "quality selection stays off for this session",
                 attempt, maxAttempts, retry::StageName(failure.stage), failure.error, failure.threadId);
    }
    void Recovered(int attempt) const { LOG_INFO("FFXIV native quality: installed on attempt {}", attempt); }
};

struct AttemptStep
{
    retry::StepResult operator()() const { return Attempt(); }
};
struct NowMs
{
    uint64_t operator()() const { return GetTickCount64(); }
};

using Installer = retry::RetryingInstaller<AttemptStep, NowMs, InstallLog>;

// A function-local static, not a namespace-scope variable: the project builds with _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR, so
// the installer (it holds a std::mutex) is not constant-initialised, and this form cannot be reached before it is built.
inline Installer& TheInstaller()
{
    static Installer installer { AttemptStep {}, NowMs {}, InstallLog {} };
    return installer;
}

// EnsureHooks (ext::game::INativeQuality): lazy, up to retry::kMaxAttempts attempts, cheap once the outcome is final (one
// atomic load, no lock, no clock). The first call installs, as before. If an attempt fails for a reason that can clear by
// itself (a game thread exiting during the enlistment, another component's Detours transaction open at that moment) the next
// calls retry it, 250 ms and then 2000 ms after the failure; the callers are the feature-create and per-evaluate paths of
// ext::upscaler (UpscalerSeams.cpp), so no extra thread or timer exists. A deterministic refusal ends after one attempt, and
// after the last failed attempt the answer stays false for the session, exactly the old "refused" state.
inline bool Install() { return TheInstaller().Ensure(); }
} // namespace ext::ffxiv::quality::FfxivNativeQualityHooks
