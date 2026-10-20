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

CreateSamplerFn g_createPlain = nullptr;                // the device's own CreateSamplerState, as its vtable gives it
CreateSamplerFn g_createTramp = nullptr;                // the trampoline of the create hook once attached
std::atomic<CreateSamplerFn> g_createCall { nullptr };  // what the engine makes variants with
std::atomic<bool> g_createHooked { false };
std::atomic<bool> g_createFailed { false };
std::atomic<bool> g_featureSeen { false };              // an upscaler feature was created: the create hook may be attached now
std::mutex g_installMutex;                              // install and uninstall only; never taken by a hook
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

void OnSamplerCreated(const D3D11_SAMPLER_DESC& desc, ID3D11SamplerState* sampler, bool bypassed)
{
    // A sampler made while a manual value is set carries that value, not the game's: it is not ours to change. A sampler the
    // fork made itself (inside a bypass scope) never takes part, but it still goes through the table: its address may be one
    // a released game sampler had, and that entry must not live on for it.
    const bool eligible = !bypassed && !ManualOverrideSet() &&
                          IsEligible(desc.MipLODBias, desc.MinLOD, desc.MaxLOD, static_cast<unsigned>(desc.Filter));

    // The bias and the eligibility come from the description the game asked for (this hook sits above upstream's, so it is
    // the game's own); the entry keeps the description the device really created, with whatever upstream changed (for
    // example the anisotropy override), because variants are made from it.
    D3D11_SAMPLER_DESC created = desc;
    sampler->GetDesc(&created);
    const auto result = g_engine.table().OnCreated(sampler, created, desc.MipLODBias, eligible);

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
    const HRESULT hr = g_createTramp(device, desc, out);
    if (SUCCEEDED(hr) && desc != nullptr && out != nullptr && *out != nullptr)
        OnSamplerCreated(*desc, *out, ext::core::HooksBypassed());
    return hr;
}

// The engine makes variants inside a bypass scope: through the trampoline (below every hook) once the create hook exists, else
// through the device's own function, where upstream's hook only passes the call on because of the scope.
HRESULT STDMETHODCALLTYPE CreateVariant(ID3D11Device* device, const D3D11_SAMPLER_DESC* desc, ID3D11SamplerState** out)
{
    ext::core::BypassScope scope;
    return g_createCall.load(std::memory_order_acquire)(device, desc, out);
}

constexpr std::array<SetSamplersFn, kStages> kSetHooks = { &HkSet<0>, &HkSet<1>, &HkSet<2>, &HkSet<3>, &HkSet<4>, &HkSet<5> };
constexpr std::array<GetSamplersFn, kStages> kGetHooks = { &HkGet<0>, &HkGet<1>, &HkGet<2>, &HkGet<3>, &HkGet<4>, &HkGet<5> };

// The hook on the device's CreateSamplerState is attached LAST and detached FIRST: upstream's own hook on the same function is
// attached when the first device is hooked and detached at the end (D3D11Hooks::Unhook), and Detours transactions on one target
// must be undone in the reverse order of their attaches. Attaching at the first upscaler create (long after upstream's hook)
// and detaching in OnUnhook (before upstream's) keeps that order.
bool InstallCreateHook()
{
    if (!g_installed || g_createHooked || g_createFailed || !g_device)
        return false;

    void** const deviceTable = *reinterpret_cast<void***>(g_device.Get());
    g_createTramp = reinterpret_cast<CreateSamplerFn>(deviceTable[kDeviceCreateSamplerStateSlot]);

    // Single-thread enlistment (see InstallBindHooks).
    LONG result = ERROR_INVALID_FUNCTION;
    if (DetourTransactionBegin() == NO_ERROR)
    {
        bool ok = DetourUpdateThread(GetCurrentThread()) == NO_ERROR;
        ok &= DetourAttach(reinterpret_cast<PVOID*>(&g_createTramp), HkCreateSamplerState) == NO_ERROR;
        if (ok)
            result = DetourTransactionCommit();
        else
            DetourTransactionAbort();
    }

    if (result != NO_ERROR)
    {
        LOG_ERROR("Bind-time mip bias: the CreateSamplerState hook was not installed ({}); samplers made from now on are not "
                  "seen, the ones already known keep working",
                  result);
        g_createFailed = true;
        return false;
    }

    g_createCall.store(g_createTramp, std::memory_order_release);
    g_createHooked = true;
    LOG_INFO("Bind-time mip bias: CreateSamplerState hook installed (after the upstream one)");
    return true;
}

bool InstallBindHooks(ID3D11Device* device)
{
    std::scoped_lock install(g_installMutex);
    if (device == nullptr || g_installed || g_failed)
        return g_installed;

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
    g_createPlain = reinterpret_cast<CreateSamplerFn>(deviceTable[kDeviceCreateSamplerStateSlot]);
    for (unsigned s = 0; s < kStages; ++s)
    {
        g_set[s] = reinterpret_cast<SetSamplersFn>(contextTable[kContextSetSamplersSlots[s]]);
        g_get[s] = reinterpret_cast<GetSamplersFn>(contextTable[kContextGetSamplersSlots[s]]);
    }

    // Single-thread enlistment, like the other device hooks of the fork (ext/ffxiv/lighting/FfxivLightingCapture.h): the
    // install happens once, early, and enlisting every thread would spread Detours' allocation-while-suspended hazard to a
    // path that runs every session.
    LONG result = ERROR_INVALID_FUNCTION;
    if (DetourTransactionBegin() == NO_ERROR)
    {
        bool ok = DetourUpdateThread(GetCurrentThread()) == NO_ERROR;
        for (unsigned s = 0; s < kStages; ++s)
        {
            ok &= DetourAttach(reinterpret_cast<PVOID*>(&g_set[s]), kSetHooks[s]) == NO_ERROR;
            ok &= DetourAttach(reinterpret_cast<PVOID*>(&g_get[s]), kGetHooks[s]) == NO_ERROR;
        }
        if (ok)
            result = DetourTransactionCommit();
        else
            DetourTransactionAbort();
    }

    if (result != NO_ERROR)
    {
        LOG_ERROR("Bind-time mip bias: hooks not installed ({}); the create-time module stays in charge", result);
        g_failed = true;
        return false;
    }

    g_createCall.store(g_createPlain, std::memory_order_release);
    g_engine.SetCreateFn(&CreateVariant);
    g_engine.Attach(context.Get(), g_set, g_get);
    g_context = context;
    g_device = device;
    g_stamp = StampContext(context.Get());
    g_installed = true;
    g_ready.store(true, std::memory_order_release);
    LOG_INFO("Bind-time mip bias: sampler hooks installed on device {:#x}, immediate context {:#x}", (size_t) device,
             (size_t) context.Get());

    if (g_featureSeen)
        InstallCreateHook();

    // The older module may already hold the override: it stands down now (restores the user's own value).
    mipbias::OnSettingChanged();
    return true;
}

void Uninstall()
{
    std::scoped_lock install(g_installMutex);
    if (!g_installed.load())
        return;

    g_ready.store(false, std::memory_order_release);
    g_engine.SetActive(false); // the game's own samplers go back on the context

    // Two transactions, in the reverse order of the attaches: the create hook first (it sits on top of upstream's).
    if (g_createHooked)
    {
        ext::core::DetourThreadScope threads;
        bool done = false;
        if (threads.Ok() && DetourTransactionBegin() == NO_ERROR)
        {
            bool ok = threads.Update() == NO_ERROR;
            ok &= DetourDetach(reinterpret_cast<PVOID*>(&g_createTramp), HkCreateSamplerState) == NO_ERROR;
            if (ok && DetourTransactionCommit() == NO_ERROR)
                done = true;
            else if (!ok)
                DetourTransactionAbort();
        }
        if (!done)
        {
            LOG_WARN("Bind-time mip bias: the CreateSamplerState hook was left attached (inactive)");
            g_ready.store(true, std::memory_order_release);
            return;
        }
        g_createHooked = false;
        g_createCall.store(g_createPlain, std::memory_order_release);
    }

    ext::core::DetourThreadScope threads;
    if (!threads.Ok())
    {
        LOG_WARN("Bind-time mip bias: unable to enlist game threads; hooks left attached (inactive)");
        g_ready.store(true, std::memory_order_release);
        return;
    }

    if (DetourTransactionBegin() != NO_ERROR)
        return;
    bool ok = threads.Update() == NO_ERROR;
    for (unsigned s = 0; s < kStages; ++s)
    {
        ok &= DetourDetach(reinterpret_cast<PVOID*>(&g_set[s]), kSetHooks[s]) == NO_ERROR;
        ok &= DetourDetach(reinterpret_cast<PVOID*>(&g_get[s]), kGetHooks[s]) == NO_ERROR;
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
        InstallBindHooks(g_device.Get());
    else
        mipbias::OnSettingChanged();
}

void OnFeatureCreate()
{
    g_featureSeen = true;
    if (g_installed.load() && !g_createHooked && !g_createFailed)
    {
        std::scoped_lock install(g_installMutex);
        InstallCreateHook();
    }
}

void OnUnhook()
{
    Uninstall();
    g_device.Reset();
}

void OnSettingChanged()
{
    if (!g_installed && !g_failed && g_ffxiv && g_device && AutoEnabled())
        InstallBindHooks(g_device.Get());
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
