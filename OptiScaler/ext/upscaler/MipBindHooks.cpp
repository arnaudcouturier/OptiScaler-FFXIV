#include "pch.h"

#include "ext/upscaler/MipBindHooks.h"

#include "ext/core/DetourThreads.h"
#include "ext/core/ExtConfig.h"
#include "ext/core/HooksBypass.h"
#include "ext/upscaler/MipBias.h"
#include "ext/upscaler/MipBindEngine.h"

#include <Config.h>
#include <upscalers/IFeature.h>

#include <atomic>
#include <mutex>

namespace ext::upscaler::mipbind
{
namespace
{
Engine g_engine;

std::atomic<bool> g_ffxiv { false };     // a matched FFXIV build's device was seen: the setting defaults to on
std::atomic<bool> g_installed { false }; // the hooks are attached and the engine is wired
std::atomic<bool> g_ready { false };     // the detour thunks may call the engine
std::atomic<bool> g_failed { false };    // installing failed once: not tried again, the create-time module takes over

ComPtr<ID3D11Device> g_device; // kept so that the menu can attach the hooks later when the setting comes on
ComPtr<ID3D11DeviceContext> g_context;

CreateSamplerFn g_create = nullptr; // the detour trampolines once attached
std::array<SetSamplersFn, kStages> g_set {};
std::array<GetSamplersFn, kStages> g_get {};

// Guards the detector, the observer and the log latches. Taken by sampler creates and by an evaluate (a few times per frame at
// most); NEVER by the bind path.
std::mutex g_mutex;
ModeDetector g_detector;
Observer g_observer;
bool g_tableFullLogged = false;

// Immediate-context timeline only (the evaluate seam): what was applied and what was logged.
bool g_identityLogged = false;
bool g_firstTarget = true;

constexpr GUID kContextTag { 0x9d0d5c3e, 0x7c1b, 0x4a62, { 0x8f, 0x37, 0x41, 0xb5, 0x0e, 0x6a, 0x2c, 0x90 } };
std::atomic<unsigned long long> g_stampSequence { 0 };
unsigned long long g_stamp = 0;

unsigned long long StampContext(ID3D11DeviceContext* context)
{
    if (context == nullptr || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
        return 0;

    const unsigned long long token = ++g_stampSequence;
    return SUCCEEDED(context->SetPrivateData(kContextTag, sizeof(token), &token)) ? token : 0;
}

bool ContextMatches(ID3D11DeviceContext* context)
{
    unsigned long long actual = 0;
    UINT size = sizeof(actual);
    return context != nullptr && g_stamp != 0 && SUCCEEDED(context->GetPrivateData(kContextTag, &size, &actual)) &&
           size == sizeof(actual) && actual == g_stamp;
}

std::optional<bool> Explicit(const CustomOptional<bool, NoDefault>& setting)
{
    return static_cast<const std::optional<bool>&>(setting);
}

bool ManualOverrideSet() { return Config::Instance()->MipmapBiasOverride.has_value(); }

// ---- detours ---------------------------------------------------------------------------------------------------------------
template <unsigned S>
void STDMETHODCALLTYPE HkSet(ID3D11DeviceContext* c, UINT start, UINT count, ID3D11SamplerState* const* input)
{
    if (!g_ready.load(std::memory_order_acquire))
    {
        g_set[S](c, start, count, input);
        return;
    }
    g_engine.Set(S, c, start, count, input);
}

template <unsigned S>
void STDMETHODCALLTYPE HkGet(ID3D11DeviceContext* c, UINT start, UINT count, ID3D11SamplerState** output)
{
    if (!g_ready.load(std::memory_order_acquire))
    {
        g_get[S](c, start, count, output);
        return;
    }
    g_engine.Get(S, c, start, count, output);
}

void OnSamplerCreated(const D3D11_SAMPLER_DESC& desc, ID3D11SamplerState* sampler)
{
    // A sampler made while a manual value is set carries that value, not the game's: it is not ours to change.
    const bool eligible =
        !ManualOverrideSet() && IsEligible(desc.MipLODBias, desc.MinLOD, desc.MaxLOD, static_cast<unsigned>(desc.Filter));

    const auto result = g_engine.table().OnCreated(sampler, desc, desc.MipLODBias, eligible);

    if (result == SamplerTable::Result::Full)
    {
        ++g_engine.counters().tableFull;
        std::scoped_lock lock(g_mutex);
        if (!g_tableFullLogged)
        {
            g_tableFullLogged = true;
            LOG_WARN("Bind-time mip bias: more than {} eligible samplers; the rest keep the game's own bias", kTableCapacity);
        }
        return;
    }

    if (!eligible || result == SamplerTable::Result::Ignored || result == SamplerTable::Result::Invalidated)
        return;

    ++g_engine.counters().eligibleCreated;

    std::scoped_lock lock(g_mutex);
    const Decision decision = g_detector.OnEligibleCreated(desc.MipLODBias);
    if (decision.decided)
        LOG_INFO("{}", DecisionLine(decision));

    if (Cfg().upscaler.mipBiasObserve.value_or_default() && g_detector.haveRatio())
        if (const auto line = g_observer.Note(g_detector.ratio(), desc.MipLODBias))
            LOG_INFO("{}", *line);
}

HRESULT STDMETHODCALLTYPE HkCreateSamplerState(ID3D11Device* device, const D3D11_SAMPLER_DESC* desc,
                                               ID3D11SamplerState** out)
{
    const HRESULT hr = g_create(device, desc, out);
    // The fork's own samplers are made inside a bypass scope (or have a bias of zero) and never take part.
    if (SUCCEEDED(hr) && desc != nullptr && out != nullptr && *out != nullptr && !ext::core::HooksBypassed())
        OnSamplerCreated(*desc, *out);
    return hr;
}

// The engine makes variants through the trampoline, below every hook, inside a bypass scope.
HRESULT STDMETHODCALLTYPE CreateVariant(ID3D11Device* device, const D3D11_SAMPLER_DESC* desc, ID3D11SamplerState** out)
{
    ext::core::BypassScope scope;
    return g_create(device, desc, out);
}

bool Install(ID3D11Device* device)
{
    if (device == nullptr)
        return false;

    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (!context)
    {
        LOG_ERROR("Bind-time mip bias: the device has no immediate context");
        g_failed = true;
        return false;
    }

    void** const deviceTable = *reinterpret_cast<void***>(device);
    void** const contextTable = *reinterpret_cast<void***>(context.Get());
    g_create = reinterpret_cast<CreateSamplerFn>(deviceTable[kDeviceCreateSamplerStateSlot]);
    for (unsigned s = 0; s < kStages; ++s)
    {
        g_set[s] = reinterpret_cast<SetSamplersFn>(contextTable[kContextSetSamplersSlots[s]]);
        g_get[s] = reinterpret_cast<GetSamplersFn>(contextTable[kContextGetSamplersSlots[s]]);
    }

    static constexpr std::array<SetSamplersFn, kStages> setHooks = { &HkSet<0>, &HkSet<1>, &HkSet<2>,
                                                                      &HkSet<3>, &HkSet<4>, &HkSet<5> };
    static constexpr std::array<GetSamplersFn, kStages> getHooks = { &HkGet<0>, &HkGet<1>, &HkGet<2>,
                                                                      &HkGet<3>, &HkGet<4>, &HkGet<5> };

    // Single-thread enlistment, like the other device hooks of the fork (ext/ffxiv/lighting/FfxivLightingCapture.h): the
    // install happens once, early, and enlisting every thread would spread Detours' allocation-while-suspended hazard to a
    // path that runs every session.
    bool began = false;
    LONG result = ERROR_INVALID_FUNCTION;
    if (DetourTransactionBegin() == NO_ERROR)
    {
        began = true;
        bool ok = DetourUpdateThread(GetCurrentThread()) == NO_ERROR;
        ok &= DetourAttach(reinterpret_cast<PVOID*>(&g_create), HkCreateSamplerState) == NO_ERROR;
        for (unsigned s = 0; s < kStages; ++s)
        {
            ok &= DetourAttach(reinterpret_cast<PVOID*>(&g_set[s]), setHooks[s]) == NO_ERROR;
            ok &= DetourAttach(reinterpret_cast<PVOID*>(&g_get[s]), getHooks[s]) == NO_ERROR;
        }
        if (ok)
            result = DetourTransactionCommit();
        else
            DetourTransactionAbort();
    }

    if (!began || result != NO_ERROR)
    {
        LOG_ERROR("Bind-time mip bias: hooks not installed ({}); the create-time module stays in charge", result);
        g_failed = true;
        return false;
    }

    g_engine.SetCreateFn(&CreateVariant);
    g_engine.Attach(context.Get(), g_set, g_get);
    g_context = context;
    g_stamp = StampContext(context.Get());
    g_installed = true;
    g_ready.store(true, std::memory_order_release);
    LOG_INFO("Bind-time mip bias: sampler hooks installed on device {:#x}, immediate context {:#x}", (size_t) device,
             (size_t) context.Get());

    // The older module may already hold the override: it stands down now (restores the user's own value).
    mipbias::OnSettingChanged();
    return true;
}

void Uninstall()
{
    if (!g_installed.load())
        return;

    g_ready.store(false, std::memory_order_release);
    g_engine.SetActive(false); // the game's own samplers go back on the context

    ext::core::DetourThreadScope threads;
    if (!threads.Ok())
    {
        LOG_WARN("Bind-time mip bias: unable to enlist game threads; hooks left attached (inactive)");
        g_ready.store(true, std::memory_order_release);
        return;
    }

    static constexpr std::array<SetSamplersFn, kStages> setHooks = { &HkSet<0>, &HkSet<1>, &HkSet<2>,
                                                                      &HkSet<3>, &HkSet<4>, &HkSet<5> };
    static constexpr std::array<GetSamplersFn, kStages> getHooks = { &HkGet<0>, &HkGet<1>, &HkGet<2>,
                                                                      &HkGet<3>, &HkGet<4>, &HkGet<5> };
    if (DetourTransactionBegin() != NO_ERROR)
        return;
    bool ok = threads.Update() == NO_ERROR;
    ok &= DetourDetach(reinterpret_cast<PVOID*>(&g_create), HkCreateSamplerState) == NO_ERROR;
    for (unsigned s = 0; s < kStages; ++s)
    {
        ok &= DetourDetach(reinterpret_cast<PVOID*>(&g_set[s]), setHooks[s]) == NO_ERROR;
        ok &= DetourDetach(reinterpret_cast<PVOID*>(&g_get[s]), getHooks[s]) == NO_ERROR;
    }
    if (ok && DetourTransactionCommit() == NO_ERROR)
    {
        g_installed = false;
        g_context.Reset();
    }
    else if (!ok)
    {
        DetourTransactionAbort();
    }
}

// Where the width the target is made from comes from: the render width of this evaluate is the smaller of the render subrect
// and Width, the output width is OutWidth, else the output texture, else the display width the feature was created with.
bool ReadSizes(NVSDK_NGX_Parameter* parameters, unsigned& renderW, unsigned& outputW)
{
    if (parameters == nullptr)
        return false;

    unsigned subrectW = 0, paramW = 0, outW = 0;
    parameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &subrectW);
    parameters->Get(NVSDK_NGX_Parameter_Width, &paramW);
    parameters->Get(NVSDK_NGX_Parameter_OutWidth, &outW);

    unsigned textureW = 0;
    unsigned createDisplayW = 0;
    if (outW == 0)
    {
        ID3D11Resource* output = nullptr;
        if (parameters->Get(NVSDK_NGX_Parameter_Output, &output) != NVSDK_NGX_Result_Success)
            parameters->Get(NVSDK_NGX_Parameter_Output, reinterpret_cast<void**>(&output));
        if (output != nullptr)
        {
            ComPtr<ID3D11Texture2D> texture;
            if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&texture))))
            {
                D3D11_TEXTURE2D_DESC desc {};
                texture->GetDesc(&desc);
                textureW = desc.Width;
            }
        }
        createDisplayW = mipbias::CurrentStatus().sizes.displayW;
    }

    renderW = PickRenderWidth(subrectW, paramW);
    outputW = PickOutputWidth(outW, textureW, createDisplayW);
    return renderW != 0 && outputW != 0;
}
} // namespace

bool Owns() { return g_installed.load(std::memory_order_relaxed); }

bool AutoEnabled()
{
    const auto& settings = Cfg().upscaler;
    return EffectiveAuto(Explicit(settings.mipBiasAuto), Explicit(settings.mipBiasAutoAlias),
                         g_ffxiv.load(std::memory_order_relaxed));
}

void OnDeviceHooked(ID3D11Device* device)
{
    if (device == nullptr)
        return;

    g_ffxiv = true;
    if (!g_device)
        g_device = device;

    // The older module's view of the setting depends on the game default: tell it now that the game is known.
    if (!g_installed && !g_failed && AutoEnabled())
        Install(g_device.Get());
    else
        mipbias::OnSettingChanged();
}

void OnUnhook()
{
    Uninstall();
    g_device.Reset();
}

void OnSettingChanged()
{
    if (!g_installed && !g_failed && g_ffxiv && g_device && AutoEnabled())
        Install(g_device.Get());
}

void OnEvaluate(ID3D11DeviceContext* context, NVSDK_NGX_Parameter* parameters)
{
    if (!g_installed.load(std::memory_order_relaxed))
        return;

    if (!g_identityLogged)
    {
        g_identityLogged = true;
        if (ContextMatches(context))
            LOG_INFO("Bind-time mip bias: the evaluate context is the hooked immediate context");
        else
            LOG_INFO("Bind-time mip bias: the evaluate context {:#x} is not marked as the hooked context {:#x} (a wrapper "
                     "that does not forward private data, or another device); binds are still served on the hooked one",
                     (size_t) context, (size_t) g_engine.Latched());
    }

    // A manual value always wins; so does switching the setting off.
    if (!BindShouldRun(AutoEnabled(), ManualOverrideSet()))
    {
        if (g_engine.Active())
        {
            g_engine.SetActive(false);
            LOG_INFO("Bind-time mip bias: off ({}); the game's own samplers are bound again",
                     ManualOverrideSet() ? "a manual Mipmap Bias value is set" : "the setting is off");
        }
        return;
    }

    unsigned renderW = 0, outputW = 0;
    if (!ReadSizes(parameters, renderW, outputW))
        return;

    const double scaling = mipbias::CurrentStatus().outputScaling;
    const auto base = BaseFromEvaluate(renderW, outputW, scaling);
    if (!base)
        return; // not a game scene: the previous target stays

    const double offset = Cfg().upscaler.mipBiasAutoOffset.value_or_default();
    const double target = Target(*base, offset);

    Mode mode;
    {
        std::scoped_lock lock(g_mutex);
        g_detector.OnRatio(*base);
        mode = g_detector.mode();
    }

    bool changed = !g_engine.HasTarget() || std::fabs(target - g_engine.Target()) >= 0.001 || mode != g_engine.CurrentMode();
    if (changed)
    {
        g_engine.SetTarget(target, mode);
        LOG_INFO("{}", TargetLine(renderW, outputW, *base, offset, target, mode, g_firstTarget));
        g_firstTarget = false;
    }

    if (!g_engine.Active())
    {
        g_engine.SetActive(true);
        changed = true;
    }

    if (changed)
        g_engine.Refresh();
}

BindStatus CurrentBindStatus()
{
    BindStatus s;
    s.installed = g_installed.load();
    s.manualWins = ManualOverrideSet();
    s.running = g_engine.Active();
    s.hasTarget = g_engine.HasTarget();
    s.target = g_engine.Target();
    s.mode = g_engine.CurrentMode();
    s.substitutedBinds = g_engine.SubstitutedBinds();
    s.variants = g_engine.counters().variantsCreated.load();
    s.tableFull = g_engine.counters().tableFull.load();
    s.failures = g_engine.counters().variantFailures.load();
    s.samplers = g_engine.table().Count();
    {
        std::scoped_lock lock(g_mutex);
        s.decided = g_detector.decided();
    }
    return s;
}
} // namespace ext::upscaler::mipbind
