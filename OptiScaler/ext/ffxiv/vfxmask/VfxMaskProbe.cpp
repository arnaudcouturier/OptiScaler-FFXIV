#include "pch.h"

#include "ext/ffxiv/vfxmask/VfxMaskProbe.h"

#include "ext/core/ExtConfig.h"
#include "ext/ffxiv/vfxmask/VfxMaskApply.h"
#include "ext/ffxiv/vfxmask/VfxMaskTexture.h"

#include <Config.h>
#include <OptiTypes.h>

#include <atomic>
#include <format>
#include <mutex>
#include <string>

namespace ext::ffxiv::vfxmask
{
namespace
{
// The armed mode: one relaxed load decides whether an evaluate does anything at all. Written by the config load, read on the
// evaluate thread.
std::atomic<std::uint8_t> g_mode { static_cast<std::uint8_t>(ProbeMode::Off) };
std::atomic<unsigned> g_arms { 0 };

struct State
{
    // The mask and the log gate belong to the evaluate path. The lock is uncontended for a game that evaluates one feature at a
    // time (the usual case) and keeps a second feature's evaluate from rebuilding the mask under the first one's hand.
    std::mutex mutex;
    MaskTexture mask;
    LogGate gate;
};

State& S()
{
    static State state;
    return state;
}

ProbeMode CurrentMode() noexcept
{
    return static_cast<ProbeMode>(g_mode.load(std::memory_order_relaxed));
}

// The upscaler this evaluate runs on: the DirectX 11 choice, which is the key the bridge's own reactive-mask requirement reads
// (upscalers/IFeature_Dx11wDx12.cpp). Only used for the log line.
const char* BackendName() noexcept
{
    switch (Config::Instance()->Dx11Upscaler.value_or_default())
    {
    case Upscaler::DLSS:
    case Upscaler::DLSS_on12: return "DLSS";
    case Upscaler::FSR21:
    case Upscaler::FSR21_on12:
    case Upscaler::FSR22:
    case Upscaler::FSR22_on12:
    case Upscaler::FSR31:
    case Upscaler::FFX:
    case Upscaler::FFX_on12: return "FSR";
    case Upscaler::XeSS:
    case Upscaler::XeSS_on12: return "XeSS";
    default: return "unknown";
    }
}

void Report(ProbeMode mode, unsigned width, unsigned height, Outcome outcome, const char* reason)
{
    const LogKey key { mode, width, height, outcome, g_arms.load(std::memory_order_relaxed) };
    if (!S().gate.Take(key))
        return;

    LOG_INFO("{}", FormatLine(key, BackendName(), reason));
}

// The game's own colour input of this evaluate. The seam is handed it, and the parameter block is the fallback for a game that
// did not pass one (the bridge reads the block the same way for every input it copies).
ID3D11Resource* ColourInput(ID3D11Resource* passed, NVSDK_NGX_Parameter* parameters)
{
    if (passed != nullptr)
        return passed;
    if (parameters == nullptr)
        return nullptr;

    ID3D11Resource* colour = nullptr;
    if (parameters->Get(NVSDK_NGX_Parameter_Color, &colour) != NVSDK_NGX_Result_Success)
        parameters->Get(NVSDK_NGX_Parameter_Color, reinterpret_cast<void**>(&colour));
    return colour;
}
} // namespace

bool Armed() noexcept
{
    return g_mode.load(std::memory_order_relaxed) != static_cast<std::uint8_t>(ProbeMode::Off);
}

void OnConfigLoaded(const char* text) noexcept
{
    try
    {
        const ProbeMode mode = ParseMode(text != nullptr ? std::string_view(text) : std::string_view());
        const ProbeMode previous = CurrentMode();

        if (mode != ProbeMode::Off && previous == ProbeMode::Off)
            g_arms.fetch_add(1, std::memory_order_relaxed);
        g_mode.store(static_cast<std::uint8_t>(mode), std::memory_order_relaxed);

        if (mode == ProbeMode::Off && previous != ProbeMode::Off)
            LOG_INFO("VFX mask probe: off, the game's own value for the mask input is used again");
    }
    catch (...)
    {
        // A config load never fails because of this key: it stays where it was.
    }
}

void OnUpscaleBoundary(ID3D11DeviceContext* context, ID3D11Resource* color,
                       NVSDK_NGX_Parameter* parameters) noexcept
{
    const ProbeMode mode = CurrentMode();
    if (mode == ProbeMode::Off)
        return;

    try
    {
        std::lock_guard<std::mutex> lock(S().mutex);

        if (parameters == nullptr)
        {
            Report(mode, 0, 0, Outcome::Refused, "the evaluate has no parameter block");
            return;
        }

        ID3D11Resource* colour = ColourInput(color, parameters);
        if (colour == nullptr)
        {
            Report(mode, 0, 0, Outcome::Refused, "the evaluate has no colour input");
            return;
        }

        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        if (FAILED(colour->QueryInterface(IID_PPV_ARGS(&texture))) || texture == nullptr)
        {
            Report(mode, 0, 0, Outcome::Refused, "the colour input is not a texture");
            return;
        }

        D3D11_TEXTURE2D_DESC colourDesc = {};
        texture->GetDesc(&colourDesc);

        // The mask is exactly the colour input's size: the bridge copies every input over unchanged, and the upscaler reads the
        // mask over the same render window it reads the colour over.
        const MaskPlan plan = PlanMask(colourDesc.Width, colourDesc.Height, mode);
        if (!plan.ok)
        {
            Report(mode, colourDesc.Width, colourDesc.Height, Outcome::Refused,
                   "the colour input's size cannot be split into two halves");
            return;
        }

        // Creates the mask once per size or mode change and validates the description before every write, so nothing that is
        // not a single-sample R8_UNORM mask of this size can reach the upscaler (a mask it cannot use fails the whole evaluate).
        const EnsureResult built = EnsureMaskTexture(context, plan.width, plan.height, mode, S().mask);
        if (built.texture == nullptr)
        {
            const std::string reason = built.error != 0
                                           ? std::format("{} ({:#x})", built.reason != nullptr ? built.reason : "unknown",
                                                         static_cast<unsigned long>(built.error))
                                           : std::string(built.reason != nullptr ? built.reason : "unknown");
            Report(mode, plan.width, plan.height, Outcome::Refused, reason.c_str());
            return;
        }

        const ApplyResult applied = ApplyBiasMask(parameters, built.texture, mode, plan.width, plan.height);
        Report(mode, plan.width, plan.height, applied.outcome, nullptr);
    }
    catch (...)
    {
        // Never into the evaluate path: one line, and this evaluate keeps the game's own value.
        Report(mode, 0, 0, Outcome::Refused, "an exception was raised while building the mask");
    }
}

namespace
{
// Registers the config-load hook when this translation unit's statics are initialized, and makes up for a config load that ran
// before that with the value the config holds (the order of static initialization across translation units is unspecified).
bool RegisterLoadHook()
{
    FfxivConfig::vfxMaskProbeOnLoaded = &OnConfigLoaded;
    OnConfigLoaded(ext::Cfg().ffxiv.vfxMaskProbe.value_or_default().c_str());
    return true;
}

[[maybe_unused]] const bool kLoadHookRegistered = RegisterLoadHook();
} // namespace
} // namespace ext::ffxiv::vfxmask
