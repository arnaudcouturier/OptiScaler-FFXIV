#include "pch.h"

#include "ext/upscaler/MipBindHooks.h"

#include "ext/core/ExtConfig.h"
#include "ext/core/HooksBypass.h"
#include "ext/upscaler/MipBias.h"
#include "ext/upscaler/MipBindControl.h"

#include <Config.h>
#include <detours/detours.h>
#include <upscalers/IFeature.h>

#include <atomic>
#include <mutex>

namespace ext::upscaler::mipbind
{
namespace
{
void Sink(LogLevel level, const std::string& text)
{
    if (level == LogLevel::Warn)
        LOG_WARN("{}", text);
    else
        LOG_INFO("{}", text);
}

Engine g_engine;
Controller g_controller(g_engine, &Sink);

std::mutex g_installMutex; // install, uninstall and the plan; never taken by a hook or by the bind path
InstallPlan g_plan;

std::atomic<bool> g_ffxiv { false };     // a matched FFXIV build's device was seen: the setting defaults to on
std::atomic<bool> g_installed { false }; // the bind hooks are attached and the engine is wired (mirrors the plan, lock-free read)
std::atomic<bool> g_ready { false };     // the detour thunks may call the engine

// What was captured at the device seam. Raw pointers on purpose: no reference is held on the game's device or context (an extra
// reference would hide the game's last release from upstream's own release hook), and nothing here is dereferenced later; the
// vtables are static data of the runtime.
struct Captured
{
    bool valid = false;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    void** deviceTable = nullptr;
    void** contextTable = nullptr;
    unsigned long long stamp = 0;
} g_cap;

CreateSamplerFn g_createPlain = nullptr;               // the device's own CreateSamplerState, as its vtable gives it
CreateSamplerFn g_createTramp = nullptr;               // the trampoline of the create hook once attached
std::atomic<CreateSamplerFn> g_createCall { nullptr }; // what the engine makes variants with
std::array<SetSamplersFn, kStages> g_set {};
std::array<GetSamplersFn, kStages> g_get {};

// Immediate-context timeline only.
bool g_identityDone = false;
unsigned g_identityChecks = 0;

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

HRESULT STDMETHODCALLTYPE HkCreateSamplerState(ID3D11Device* device, const D3D11_SAMPLER_DESC* desc,
                                               ID3D11SamplerState** out)
{
    const HRESULT hr = g_createTramp(device, desc, out);
    // The fork's own samplers (made inside a bypass scope) never take part but still refresh the table.
    if (SUCCEEDED(hr) && desc != nullptr && out != nullptr && *out != nullptr)
        g_controller.OnSamplerCreated(*out, device, *desc, ext::core::HooksBypassed());
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

// Detours enlistment: the current thread only, for the attaches and for the detaches. Enlisting every other thread suspends them
// while Detours allocates and frees (ext/core/DetourThreads.h documents the heap-lock hazard as real, and the reviews of the
// other ext hooks asked for the live paths to avoid it). What is accepted in exchange: a thread that is inside the first bytes
// of a patched function at that very moment can crash. The attaches happen at quiet moments: the device seam, the first
// upscaler create, and a menu click. A detach failure leaves the hooks attached and inert.
LONG RunTransaction(bool attach, std::initializer_list<std::pair<PVOID*, PVOID>> targets)
{
    if (DetourTransactionBegin() != NO_ERROR)
        return ERROR_INVALID_FUNCTION;

    bool ok = DetourUpdateThread(GetCurrentThread()) == NO_ERROR;
    for (const auto& t : targets)
        ok &= (attach ? DetourAttach(t.first, t.second) : DetourDetach(t.first, t.second)) == NO_ERROR;

    if (!ok)
    {
        DetourTransactionAbort();
        return ERROR_INVALID_FUNCTION;
    }
    return DetourTransactionCommit();
}

void Capture(ID3D11Device* device)
{
    if (g_cap.valid || device == nullptr)
        return;

    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (!context)
    {
        LOG_ERROR("Bind-time mip bias: the device has no immediate context");
        return;
    }

    g_cap.device = device;
    g_cap.context = context.Get();
    g_cap.deviceTable = *reinterpret_cast<void***>(device);
    g_cap.contextTable = *reinterpret_cast<void***>(context.Get());
    g_cap.stamp = ContextMarker::Stamp(context.Get());
    g_cap.valid = true;
    LOG_INFO("Bind-time mip bias: device {:#x} and immediate context {:#x} noted (no reference held)", (size_t) device,
             (size_t) context.Get());
}

// The hook on the device's CreateSamplerState is attached LAST and detached FIRST: upstream's own hook on the same function is
// attached when the first device is hooked and detached at the end (D3D11Hooks::Unhook), and Detours transactions on one target
// must be undone in the reverse order of their attaches. Attaching at the first upscaler create (long after upstream's hook) and
// detaching in OnUnhook (before upstream's) keeps that order. Caller holds g_installMutex.
void InstallCreateHook()
{
    g_createTramp = reinterpret_cast<CreateSamplerFn>(g_cap.deviceTable[kDeviceCreateSamplerStateSlot]);
    const LONG result = RunTransaction(true, { { reinterpret_cast<PVOID*>(&g_createTramp), reinterpret_cast<PVOID>(HkCreateSamplerState) } });
    g_plan.CreateResult(result == NO_ERROR);
    if (result != NO_ERROR)
    {
        LOG_ERROR("Bind-time mip bias: the CreateSamplerState hook was not installed ({}); samplers made from now on are seen "
                  "only when they are first bound",
                  result);
        return;
    }

    g_createCall.store(g_createTramp, std::memory_order_release);
    LOG_INFO("Bind-time mip bias: CreateSamplerState hook installed (after the upstream one)");
}

// Caller holds g_installMutex.
void InstallBindHooks()
{
    if (!g_cap.valid)
    {
        g_plan.BindResult(false);
        return;
    }

    g_createPlain = reinterpret_cast<CreateSamplerFn>(g_cap.deviceTable[kDeviceCreateSamplerStateSlot]);
    for (unsigned s = 0; s < kStages; ++s)
    {
        g_set[s] = reinterpret_cast<SetSamplersFn>(g_cap.contextTable[kContextSetSamplersSlots[s]]);
        g_get[s] = reinterpret_cast<GetSamplersFn>(g_cap.contextTable[kContextGetSamplersSlots[s]]);
    }

    LONG result = ERROR_INVALID_FUNCTION;
    if (DetourTransactionBegin() == NO_ERROR)
    {
        bool ok = DetourUpdateThread(GetCurrentThread()) == NO_ERROR;
        for (unsigned s = 0; s < kStages; ++s)
        {
            ok &= DetourAttach(reinterpret_cast<PVOID*>(&g_set[s]), reinterpret_cast<PVOID>(kSetHooks[s])) == NO_ERROR;
            ok &= DetourAttach(reinterpret_cast<PVOID*>(&g_get[s]), reinterpret_cast<PVOID>(kGetHooks[s])) == NO_ERROR;
        }
        if (ok)
            result = DetourTransactionCommit();
        else
            DetourTransactionAbort();
    }

    if (result != NO_ERROR)
    {
        LOG_ERROR("Bind-time mip bias: hooks not installed ({}); the create-time module stays in charge", result);
        g_plan.BindResult(false);
        return;
    }

    g_createCall.store(g_createPlain, std::memory_order_release);
    g_engine.SetCreateFn(&CreateVariant);
    g_engine.Attach(g_cap.context, g_set, g_get);
    g_installed = true;
    g_ready.store(true, std::memory_order_release);
    LOG_INFO("Bind-time mip bias: sampler hooks installed on device {:#x}, immediate context {:#x}", (size_t) g_cap.device,
             (size_t) g_cap.context);

    if (g_plan.BindResult(true) == InstallPlan::Action::InstallCreate)
        InstallCreateHook();
}

void Perform(InstallPlan::Action action)
{
    if (action == InstallPlan::Action::InstallBind)
    {
        InstallBindHooks();
        // The older module may hold the override: it stands down now (restores the user's own value), or follows the setting
        // with the game known when the bind-time hooks did not come up.
        mipbias::OnSettingChanged();
    }
    else if (action == InstallPlan::Action::InstallCreate)
    {
        InstallCreateHook();
    }
}

void Uninstall()
{
    std::scoped_lock install(g_installMutex);
    if (!g_installed.load())
        return;

    g_ready.store(false, std::memory_order_release);
    g_engine.SetActive(false); // the game's own samplers go back on the context

    // Two transactions, in the reverse order of the attaches: the create hook first (it sits on top of upstream's).
    if (g_plan.createHooked())
    {
        if (RunTransaction(false, { { reinterpret_cast<PVOID*>(&g_createTramp), reinterpret_cast<PVOID>(HkCreateSamplerState) } }) != NO_ERROR)
        {
            LOG_WARN("Bind-time mip bias: the CreateSamplerState hook was left attached (inactive)");
            g_ready.store(true, std::memory_order_release);
            return;
        }
        g_plan.CreateRemoved();
        g_createCall.store(g_createPlain, std::memory_order_release);
    }

    LONG result = ERROR_INVALID_FUNCTION;
    if (DetourTransactionBegin() == NO_ERROR)
    {
        bool ok = DetourUpdateThread(GetCurrentThread()) == NO_ERROR;
        for (unsigned s = 0; s < kStages; ++s)
        {
            ok &= DetourDetach(reinterpret_cast<PVOID*>(&g_set[s]), reinterpret_cast<PVOID>(kSetHooks[s])) == NO_ERROR;
            ok &= DetourDetach(reinterpret_cast<PVOID*>(&g_get[s]), reinterpret_cast<PVOID>(kGetHooks[s])) == NO_ERROR;
        }
        if (ok)
            result = DetourTransactionCommit();
        else
            DetourTransactionAbort();
    }

    if (result != NO_ERROR)
    {
        LOG_WARN("Bind-time mip bias: the sampler hooks were left attached (inactive)");
        g_ready.store(true, std::memory_order_release);
        return;
    }

    g_plan.BindRemoved();
    g_installed = false;
    g_engine.ReleaseAll(); // no reference on a game object outlives the device
    g_cap = {};
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

// Is the evaluate's context the one the hooks serve? Logs what it finds (once), and returns an immediate context of the hooked
// class that is NOT the latched one: then the hooks sit on the wrong object (a probe device was first) and move to it.
ID3D11DeviceContext* CheckIdentity(ID3D11DeviceContext* context)
{
    if (g_identityDone || context == nullptr)
        return nullptr;

    if (ContextMarker::Matches(context, g_cap.stamp))
    {
        g_identityDone = true;
        LOG_INFO("Bind-time mip bias: the evaluate context is the hooked immediate context");
        return nullptr;
    }

    // Not marked. A ReShade-style wrapper that does not forward private data looks the same as a different device, so tell
    // them apart by the class: the hooked class has the very function the hooks patched in its vtable.
    const bool sameClass = context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE &&
                           (*reinterpret_cast<void***>(context))[kContextSetSamplersSlots[0]] ==
                               g_cap.contextTable[kContextSetSamplersSlots[0]];
    if (++g_identityChecks == 1)
    {
        ComPtr<ID3D11Device> device;
        context->GetDevice(&device);
        LOG_WARN("Bind-time mip bias: the evaluate context {:#x} (device {:#x}) is not the hooked context {:#x} (device {:#x}); "
                 "{}",
                 (size_t) context, (size_t) device.Get(), (size_t) g_engine.Latched(), (size_t) g_cap.device,
                 sameClass ? "it is of the same kind, so the hooks move to it"
                           : "it is of another kind (a wrapper that does not forward private data?): binds stay served on the "
                             "hooked one");
    }

    if (sameClass)
    {
        g_identityDone = true;
        return context;
    }
    if (g_identityChecks >= 600)
        g_identityDone = true; // enough looking
    return nullptr;
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
    {
        std::scoped_lock install(g_installMutex);
        Capture(device);
        Perform(g_plan.OnDeviceHooked(AutoEnabled()));
    }

    if (!g_installed)
        mipbias::OnSettingChanged(); // off, or the install failed: the older module follows the setting with the game known
}

void OnFeatureCreate()
{
    std::scoped_lock install(g_installMutex);
    Perform(g_plan.OnFeatureCreate());
}

void OnUnhook() { Uninstall(); }

void OnSettingChanged()
{
    std::scoped_lock install(g_installMutex);
    Perform(g_plan.OnSettingChanged(AutoEnabled()));
}

void OnEvaluate(ID3D11DeviceContext* context, NVSDK_NGX_Parameter* parameters)
{
    if (!g_installed.load(std::memory_order_relaxed))
        return;

    Settings settings;
    settings.autoEnabled = AutoEnabled();
    settings.manualOverride = ManualOverrideSet();
    settings.offset = Cfg().upscaler.mipBiasAutoOffset.value_or_default();
    settings.observe = Cfg().upscaler.mipBiasObserve.value_or_default();
    settings.outputScaling = mipbias::CurrentStatus().outputScaling;

    EvalInput input;
    input.nowMs = GetTickCount64();
    input.relatchTo = CheckIdentity(context);
    if (!ReadSizes(parameters, input.renderW, input.outputW))
    {
        input.renderW = input.outputW = 0; // the controller still runs: a hand-set value or the setting may have changed
    }

    g_controller.OnEvaluate(settings, input);

    if (input.relatchTo != nullptr)
    {
        g_cap.context = input.relatchTo;
        g_cap.stamp = ContextMarker::Stamp(input.relatchTo);
    }
}

BindStatus CurrentBindStatus() { return g_controller.Status(g_installed.load(), ManualOverrideSet()); }
} // namespace ext::upscaler::mipbind
