#include "pch.h"

#include "ext/reshade/ReShadeAddon.h"
#include "ext/reshade/ReShadeBoundaryCore.h"
#include "ext/core/HooksBypass.h"
#include "ext/reshade/ReShadeD3D11.h"
#include "ext/reshade/ReShadeDepthBackend.h"
#include "ext/reshade/ReShadeEvents.h"
#include "ext/window/WindowSeams.h"

// ReShade's headers and ImGui never share a translation unit (see ReShadeRegister.cpp).
#ifdef IMGUI_VERSION_NUM
#error "ReShadeEvents.cpp must not see imgui.h"
#endif

#include "ext/reshade/third_party/reshade/reshade.hpp"

#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <format>

namespace ext::reshade
{
namespace
{
using namespace ::reshade::api;
using probe::EffectsEvent;
using Microsoft::WRL::ComPtr;

probe::Session* g_session = nullptr; // set once by InstallProbe, never freed
probe::FileReport* g_report = nullptr;
std::atomic<bool> g_installed { false };

std::atomic<bool> g_begun { false };
std::string* g_beginLine = nullptr; // heap, never freed; set before g_session is published

// Callbacks may still arrive while the process shuts down: upstream sets isShuttingDown before it frees ReShade.
// The first callback also writes the report header: InstallProbe runs inside DllMain (loader lock), where the report folder
// and file must not be created.
bool Live()
{
    if (g_session == nullptr || State::Instance().isShuttingDown)
        return false;
    if (!g_begun.load(std::memory_order_acquire) && !g_begun.exchange(true))
        g_session->Begin(g_beginLine != nullptr ? *g_beginLine : std::string());
    return true;
}

int64_t NowQpc()
{
    LARGE_INTEGER v {};
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

uint64_t Id(effect_runtime* runtime) { return reinterpret_cast<uint64_t>(runtime); }

// Size / format of the resource a view points to (0 when it cannot be resolved).
void ResolveView(effect_runtime* runtime, resource_view view, uint32_t& w, uint32_t& h, uint32_t& format)
{
    w = h = format = 0;
    try
    {
        if (runtime == nullptr || view.handle == 0)
            return;
        device* dev = runtime->get_device();
        if (dev == nullptr)
            return;
        const resource res = dev->get_resource_from_view(view);
        if (res.handle == 0)
            return;
        const resource_desc desc = dev->get_resource_desc(res);
        if (desc.type != resource_type::texture_2d)
            return;
        w = desc.texture.width;
        h = desc.texture.height;
        format = static_cast<uint32_t>(desc.texture.format);
    }
    catch (...)
    {
    }
}

probe::Api ToApi(device_api api)
{
    switch (api)
    {
    case device_api::d3d11:
        return probe::Api::D3D11;
    case device_api::d3d12:
        return probe::Api::D3D12;
    default:
        return probe::Api::Other;
    }
}

// effect_runtime::render_effects and the state restore run through the SEH-guarded call (no C++ objects in the thunks).
struct RenderArgs
{
    effect_runtime* runtime;
    command_list* list;
    resource_view rtv;
    resource_view rtvSrgb;
};
void RenderThunk(void* p)
{
    auto* a = static_cast<RenderArgs*>(p);
    a->runtime->render_effects(a->list, a->rtv, a->rtvSrgb);
}

struct ApplyArgs
{
    d3d11::StateBlock* state;
    ID3D11DeviceContext* context;
};
void ApplyThunk(void* p)
{
    auto* a = static_cast<ApplyArgs*>(p);
    a->state->Apply(a->context);
}

// Formats ReShade (and DXGI) use for depth buffers and the single-channel float copies of them. One list, shared with the automatic
// ReShade depth backend (ReShadeDepthBackend.h, whose raw layer pins the numeric values against ReShade's format enum).
bool IsDepthFormat(format f) { return depth::IsDepthFormatValue(static_cast<uint32_t>(f)); }

class LiveBackend final : public probe::IBackend
{
  public:
    int64_t Qpc() override { return NowQpc(); }

    int64_t QpcFrequency() override
    {
        LARGE_INTEGER f {};
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }

    bool Describe(uint64_t id, bool withTechniques, probe::RuntimeInfo& out) override
    {
        out = {};
        out.id = id;
        try
        {
            auto* runtime = reinterpret_cast<effect_runtime*>(id);
            device* dev = runtime->get_device();
            if (dev == nullptr)
                return false;

            out.api = ToApi(dev->get_api());
            out.native = runtime->get_native();
            out.device = dev->get_native();
            out.hwnd = reinterpret_cast<uint64_t>(runtime->get_hwnd());
            out.backBufferCount = runtime->get_back_buffer_count();
            if (out.backBufferCount != 0)
            {
                const resource bb = runtime->get_back_buffer(0);
                if (bb.handle != 0)
                {
                    const resource_desc d = dev->get_resource_desc(bb);
                    out.backBufferWidth = d.texture.width;
                    out.backBufferHeight = d.texture.height;
                    out.backBufferFormat = static_cast<uint32_t>(d.texture.format);
                }
            }
            out.effectsEnabled = runtime->get_effects_state();

            char path[1024] = {};
            size_t size = sizeof(path);
            runtime->get_current_preset_path(path, &size);
            path[sizeof(path) - 1] = '\0';
            out.presetPath = path;

            if (withTechniques)
            {
                out.techniquesKnown = true;
                runtime->enumerate_techniques(nullptr,
                                              [&](effect_runtime* rt, effect_technique technique)
                                              {
                                                  probe::TechniqueInfo info;
                                                  char name[256] = {};
                                                  size_t nameSize = sizeof(name);
                                                  rt->get_technique_name(technique, name, &nameSize);
                                                  name[sizeof(name) - 1] = '\0';
                                                  info.name = name;
                                                  char effect[260] = {};
                                                  size_t effectSize = sizeof(effect);
                                                  rt->get_technique_effect_name(technique, effect, &effectSize);
                                                  effect[sizeof(effect) - 1] = '\0';
                                                  info.effect = effect;
                                                  info.enabled = rt->get_technique_state(technique);
                                                  out.techniques.push_back(std::move(info));
                                              });
            }
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    // ---- invasive probe steps (boundary thread) -----------------------------------------------------------------------
    uint64_t PickRuntime(void* context, const std::vector<uint64_t>& alive, std::string& why) override
    {
        try
        {
            auto* ctx = static_cast<ID3D11DeviceContext*>(context);
            ComPtr<ID3D11Device> dev;
            if (ctx != nullptr)
                ctx->GetDevice(&dev);

            uint64_t onlyD3D11 = 0;
            int d3d11Count = 0;
            for (uint64_t id : alive)
            {
                device* d = reinterpret_cast<effect_runtime*>(id)->get_device();
                if (d == nullptr || d->get_api() != device_api::d3d11)
                    continue;
                d3d11Count++;
                onlyD3D11 = id;
                if (d->get_native() == reinterpret_cast<uint64_t>(dev.Get()))
                {
                    why = std::format("runtime {:#x} owns the context's device {:#x}", id, d->get_native());
                    return id;
                }
            }
            if (d3d11Count == 1)
            {
                why = std::format("the only D3D11 runtime {:#x} (its device {:#x} differs from the context's device {:#x})", onlyD3D11,
                                  reinterpret_cast<effect_runtime*>(onlyD3D11)->get_device()->get_native(),
                                  reinterpret_cast<uint64_t>(dev.Get()));
                return onlyD3D11;
            }
            why = std::format("{} D3D11 runtimes alive, none owns the context's device {:#x}", d3d11Count,
                              reinterpret_cast<uint64_t>(dev.Get()));
        }
        catch (...)
        {
            why = "exception while matching the runtime";
        }
        return 0;
    }

    probe::BoundaryInfo Inspect(void* context, uint64_t runtime) override
    {
        _rtv.Reset();
        _rtvSrgb.Reset();
        _target.Reset();

        probe::BoundaryInfo b;
        b.context = reinterpret_cast<uint64_t>(context);
        try
        {
            auto* ctx = static_cast<ID3D11DeviceContext*>(context);
            auto* rt = reinterpret_cast<effect_runtime*>(runtime);
            if (ctx == nullptr)
            {
                b.refusal = "null context";
                return b;
            }

            command_queue* queue = rt->get_command_queue();
            command_list* list = queue != nullptr ? queue->get_immediate_command_list() : nullptr;
            if (list == nullptr)
            {
                b.refusal = "the runtime has no immediate command list";
                return b;
            }
            b.cmdListNative = list->get_native();
            if (b.cmdListNative != b.context)
            {
                b.refusal = std::format("ReShade's immediate command list wraps context {:#x}, the probe was called with {:#x}",
                                        b.cmdListNative, b.context);
                return b;
            }

            ComPtr<ID3D11RenderTargetView> bound;
            ctx->OMGetRenderTargets(1, &bound, nullptr);
            if (bound == nullptr)
            {
                b.refusal = "no render target is bound";
                return b;
            }
            ComPtr<ID3D11Resource> resource;
            bound->GetResource(&resource);
            ComPtr<ID3D11Texture2D> texture;
            if (resource == nullptr || FAILED(resource.As(&texture)))
            {
                b.refusal = "the bound render target is not a 2D texture";
                return b;
            }
            D3D11_TEXTURE2D_DESC td {};
            texture->GetDesc(&td);
            b.resource = reinterpret_cast<uint64_t>(resource.Get());
            b.width = td.Width;
            b.height = td.Height;
            b.format = static_cast<uint32_t>(td.Format);
            b.samples = td.SampleDesc.Count;
            b.arraySize = td.ArraySize;

            device* dev = rt->get_device();
            const uint32_t count = rt->get_back_buffer_count();
            resource_desc bbDesc {};
            for (uint32_t i = 0; i < count; i++)
            {
                const ::reshade::api::resource bb = rt->get_back_buffer(i);
                if (i == 0 && bb.handle != 0)
                    bbDesc = dev->get_resource_desc(bb);
                b.isRuntimeBackBuffer = b.isRuntimeBackBuffer || bb.handle == b.resource;
            }

            if (td.SampleDesc.Count != 1 || td.ArraySize != 1)
                b.refusal = std::format("the target is multisampled or an array ({} samples, {} slices)", td.SampleDesc.Count, td.ArraySize);
            else if (td.Width != bbDesc.texture.width || td.Height != bbDesc.texture.height)
                b.refusal = std::format("the target is {}x{} but the runtime's back buffer is {}x{} (effects are sized for the back buffer)",
                                        td.Width, td.Height, bbDesc.texture.width, bbDesc.texture.height);
            else if (!d3d11::SameFamily(td.Format, static_cast<DXGI_FORMAT>(bbDesc.texture.format)))
                b.refusal = std::format("the target format {} is not in the format family of the back buffer ({})", int(td.Format),
                                        static_cast<uint32_t>(bbDesc.texture.format));
            if (!b.refusal.empty())
                return b;

            // The two views ReShade wants: a linear one and an sRGB one of the same resource.
            D3D11_RENDER_TARGET_VIEW_DESC bd {};
            bound->GetDesc(&bd);
            const d3d11::FormatFamily family = d3d11::FamilyOf(td.Format);
            const DXGI_FORMAT linearFormat = family.known ? family.linear : td.Format;
            const DXGI_FORMAT srgbFormat = family.known ? family.srgb : td.Format;
            ComPtr<ID3D11Device> d3dDevice;
            ctx->GetDevice(&d3dDevice);

            auto makeView = [&](DXGI_FORMAT format, ComPtr<ID3D11RenderTargetView>& out) -> HRESULT
            {
                if (bd.Format == format && bd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D)
                {
                    out = bound;
                    return S_OK;
                }
                D3D11_RENDER_TARGET_VIEW_DESC vd {};
                vd.Format = format;
                vd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                vd.Texture2D.MipSlice = 0;
                return d3dDevice->CreateRenderTargetView(resource.Get(), &vd, &out);
            };

            const HRESULT hrLinear = makeView(linearFormat, _rtv);
            std::string note;
            if (FAILED(hrLinear))
            {
                b.refusal = std::format("could not create the linear render target view (format {}, hr {:#x})", int(linearFormat), uint32_t(hrLinear));
                return b;
            }
            note = std::format("rtv: format {} ({})", int(linearFormat), bd.Format == linearFormat ? "the game's own view" : "created");
            if (!family.known)
            {
                _rtvSrgb = _rtv;
                note += std::format("; rtv_srgb: same view (format {} has no sRGB variant)", int(td.Format));
            }
            else
            {
                const HRESULT hrSrgb = makeView(srgbFormat, _rtvSrgb);
                if (FAILED(hrSrgb))
                {
                    _rtvSrgb = _rtv;
                    note += std::format("; rtv_srgb: sRGB view creation failed (hr {:#x}), the linear view is passed instead",
                                        uint32_t(hrSrgb));
                }
                else
                    note += std::format("; rtv_srgb: format {} ({})", int(srgbFormat),
                                        bd.Format == srgbFormat ? "the game's own view" : "created");
            }
            b.note = note;
            b.rtv = reinterpret_cast<uint64_t>(_rtv.Get());
            b.rtvSrgb = reinterpret_cast<uint64_t>(_rtvSrgb.Get());
            _target = resource;
            b.ok = true;
        }
        catch (...)
        {
            b.ok = false;
            b.refusal = "exception while inspecting the bound render target";
        }
        return b;
    }

    probe::DepthReport QueryDepth(uint64_t runtime, const probe::BoundaryInfo&) override
    {
        probe::DepthReport report;
        try
        {
            auto* rt = reinterpret_cast<effect_runtime*>(runtime);
            device* dev = rt->get_device();
            size_t total = 0, named = 0;
            std::vector<std::string> lines;
            uint32_t bbWidth = 0, bbHeight = 0;
            if (rt->get_back_buffer_count() != 0)
            {
                const ::reshade::api::resource bb = rt->get_back_buffer(0);
                if (bb.handle != 0)
                {
                    const resource_desc bd = dev->get_resource_desc(bb);
                    bbWidth = bd.texture.width;
                    bbHeight = bd.texture.height;
                }
            }
            rt->enumerate_texture_variables(
                nullptr,
                [&](effect_runtime* r, effect_texture_variable variable)
                {
                    total++;
                    char name[256] = {};
                    size_t nameSize = sizeof(name);
                    r->get_texture_variable_name(variable, name, &nameSize);
                    name[sizeof(name) - 1] = '\0';
                    std::string lower = name;
                    for (char& c : lower)
                        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (lower.find("depth") == std::string::npos)
                        return;
                    named++;
                    if (lines.size() >= 24)
                        return;

                    resource_view srv {}, srvSrgb {};
                    r->get_texture_binding(variable, &srv, &srvSrgb);
                    char effect[260] = {};
                    size_t effectSize = sizeof(effect);
                    r->get_texture_variable_effect_name(variable, effect, &effectSize);
                    effect[sizeof(effect) - 1] = '\0';

                    std::string where = "no view bound";
                    if (srv.handle != 0)
                    {
                        const resource res = dev->get_resource_from_view(srv);
                        const resource_desc d = res.handle != 0 ? dev->get_resource_desc(res) : resource_desc {};
                        const bool depthFormat = IsDepthFormat(d.texture.format);
                        where = std::format("srv {:#x} -> resource {:#x} {}x{} format {}{}", srv.handle, res.handle, d.texture.width,
                                            d.texture.height, static_cast<uint32_t>(d.texture.format),
                                            depthFormat ? " (depth/single-channel float family)" : "");
                        if (res.handle != 0 && depthFormat)
                        {
                            const bool sameSize = d.texture.width == bbWidth && d.texture.height == bbHeight;
                            if (!report.anyDepthBound || (sameSize && !report.sizeMatchesBackBuffer))
                                report.summary = std::format("{} -> {}x{} format {} (back buffer {}x{})", name, d.texture.width,
                                                             d.texture.height, static_cast<uint32_t>(d.texture.format), bbWidth, bbHeight);
                            report.anyDepthBound = true;
                            report.sizeMatchesBackBuffer = report.sizeMatchesBackBuffer || sameSize;
                        }
                    }
                    lines.push_back(std::format("[F4]   {} ({}): {}", name, effect, where));
                });
            report.lines.push_back(std::format(
                "[F4] {} texture variables in the loaded effects, {} with 'depth' in the name. The ReShade API has no direct depth-"
                "detection query: this is INFERRED from what is bound to those variables (ReShade binds its detected depth "
                "buffer to the variable with the DEPTH semantic, usually DepthBufferTex). First {} listed:",
                total, named, lines.size()));
            for (auto& l : lines)
                report.lines.push_back(std::move(l));
        }
        catch (...)
        {
            report.lines.push_back("[F4] exception while reading the texture bindings");
        }
        return report;
    }

    void SnapshotBefore(void* context, const probe::BoundaryInfo&) override
    {
        try
        {
            std::string note;
            _snapshot.Capture(static_cast<ID3D11DeviceContext*>(context), _target.Get(), note);
        }
        catch (...)
        {
        }
    }

    probe::PixelDiff SnapshotAfterAndDiff(void* context, const probe::BoundaryInfo&) override
    {
        probe::PixelDiff diff;
        try
        {
            diff = _snapshot.CaptureAndDiff(static_cast<ID3D11DeviceContext*>(context), _target.Get());
        }
        catch (...)
        {
            diff.note = "exception during the comparison";
        }
        _rtv.Reset();
        _rtvSrgb.Reset();
        _target.Reset();
        return diff;
    }

    probe::CallOutcome RenderEffects(void* context, uint64_t runtime, const probe::BoundaryInfo& boundary) override
    {
        probe::CallOutcome o;
        try
        {
            auto* ctx = static_cast<ID3D11DeviceContext*>(context);
            auto* rt = reinterpret_cast<effect_runtime*>(runtime);
            command_queue* queue = rt->get_command_queue();
            command_list* list = queue != nullptr ? queue->get_immediate_command_list() : nullptr;
            if (!boundary.ok || ctx == nullptr || list == nullptr || list->get_native() != reinterpret_cast<uint64_t>(ctx))
            {
                o.what = "preconditions no longer hold (boundary, context or immediate command list)";
                return o;
            }

            ext::core::BypassScope bypass; // ReShade's own draws and copies must not look like game draws to our hooks
            d3d11::StateBlock state;
            state.Capture(ctx);

            RenderArgs args { rt, list, resource_view { boundary.rtv }, resource_view { boundary.rtvSrgb } };
            const uint32_t code = d3d11::GuardedCall(&RenderThunk, &args);

            ApplyArgs apply { &state, ctx };
            const uint32_t applyCode = d3d11::GuardedCall(&ApplyThunk, &apply);
            o.stateRestored = applyCode == 0;
            o.stateNote = applyCode == 0 ? "D3D11 state block (IA, 6 shader stages with CB/SRV/samplers, RS, OM, CS UAVs) re-applied"
                                         : std::format("state restore raised {:#x}", applyCode);

            if (code != 0)
            {
                o.exception = true;
                o.code = code;
                o.what = code == 0xC0000005u ? "access violation" : "exception";
                return o;
            }
            o.ok = true;
        }
        catch (...)
        {
            o.exception = true;
            o.what = "C++ exception";
        }
        return o;
    }

    bool GetEffectsState(uint64_t runtime, bool& enabled) override
    {
        try
        {
            enabled = reinterpret_cast<effect_runtime*>(runtime)->get_effects_state();
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool SetEffectsState(uint64_t runtime, bool enabled) override
    {
        try
        {
            reinterpret_cast<effect_runtime*>(runtime)->set_effects_state(enabled);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    // ---- S10 package E (live boundary)
    bool CountTechniques(uint64_t runtime, uint32_t& total, uint32_t& enabled) override
    {
        total = enabled = 0;
        try
        {
            auto* rt = reinterpret_cast<effect_runtime*>(runtime);
            uint32_t t = 0, e = 0;
            rt->enumerate_techniques(nullptr,
                                     [&](effect_runtime* r, effect_technique technique)
                                     {
                                         ++t;
                                         if (r->get_technique_state(technique))
                                             ++e;
                                     });
            total = t;
            enabled = e;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    void EndBoundary() override
    {
        _rtv.Reset();
        _rtvSrgb.Reset();
        _target.Reset();
    }

  private:
    ComPtr<ID3D11RenderTargetView> _rtv;
    ComPtr<ID3D11RenderTargetView> _rtvSrgb;
    ComPtr<ID3D11Resource> _target;
    d3d11::TargetSnapshot _snapshot;
};

// ---- ReShade callbacks (noexcept: nothing may escape into ReShade or the game) -------------------------------------
void OnInitRuntime(effect_runtime* runtime) noexcept
{
    if (Live())
        g_session->OnInitRuntime(Id(runtime));
}

void OnDestroyRuntime(effect_runtime* runtime) noexcept
{
    if (Live())
        g_session->OnDestroyRuntime(Id(runtime));
}

void OnReloadedEffects(effect_runtime* runtime) noexcept
{
    if (Live())
        g_session->OnReloadedEffects(Id(runtime));
}

void OnPresetPath(effect_runtime* runtime, const char* path) noexcept
{
    if (Live())
        g_session->OnPresetPath(Id(runtime), path);
}

void OnEffects(bool begin, effect_runtime* runtime, command_list* cmd, resource_view rtv, resource_view rtvSrgb) noexcept
{
    if (!Live())
        return;
    EffectsEvent e;
    e.begin = begin;
    e.runtime = Id(runtime);
    e.cmdList = reinterpret_cast<uint64_t>(cmd);
    e.rtv = rtv.handle;
    e.rtvSrgb = rtvSrgb.handle;
    ResolveView(runtime, rtv, e.width, e.height, e.format);
    e.qpc = NowQpc();
    e.fromProbe = probe::InProbeCall();
    g_session->OnEffects(e);
}

void OnBeginEffects(effect_runtime* runtime, command_list* cmd, resource_view rtv, resource_view rtvSrgb) noexcept
{
    OnEffects(true, runtime, cmd, rtv, rtvSrgb);
}

void OnFinishEffects(effect_runtime* runtime, command_list* cmd, resource_view rtv, resource_view rtvSrgb) noexcept
{
    OnEffects(false, runtime, cmd, rtv, rtvSrgb);
}

void OnReshadePresent(effect_runtime* runtime) noexcept
{
    if (Live())
        g_session->OnReshadePresent(Id(runtime));
}

void OnQueuePresent(command_queue* queue, swapchain*, const rect*, const rect*, uint32_t, const rect*) noexcept
{
    if (!Live())
        return;
    g_session->OnQueuePresent();

#ifdef HUDLESS_RESHADE_PROBE_DEBUG
    // DEBUG TRIGGER (compiled only with HUDLESS_RESHADE_PROBE_DEBUG): the start of the game's Present stands in for the HUD
    // boundary until package A's detector calls ext::reshade::ProbeAtBoundary at the real one. The render target bound at
    // Present time is not the scene target, so only the mechanics of the probe are exercised, not its meaning.
    if (queue != nullptr && g_session->Armed())
        g_session->ProbeAtBoundary(reinterpret_cast<void*>(queue->get_native()));
#else
    (void)queue;
#endif
}

// ---- S10 package E: runtime tracking for the live boundary (no probe, no report) -------------------------------------------------
bool BoundaryLive() { return !State::Instance().isShuttingDown; }

void BoundaryInit(effect_runtime* runtime) noexcept
{
    if (BoundaryLive())
        boundary::Instance().OnInitRuntime(Id(runtime));
}
void BoundaryDestroy(effect_runtime* runtime) noexcept
{
    if (BoundaryLive())
        boundary::Instance().OnDestroyRuntime(Id(runtime));
}
void BoundaryReloaded(effect_runtime* runtime) noexcept
{
    if (BoundaryLive())
        boundary::Instance().OnReloadedEffects(Id(runtime));
}
void BoundaryPresetPath(effect_runtime*, const char*) noexcept
{
    if (BoundaryLive())
        boundary::Instance().OnTechniquesChanged();
}
// Fires BEFORE the change and returns true to veto it: this one never vetoes, it only marks the technique count dirty.
bool BoundaryTechniqueState(effect_runtime*, effect_technique, bool) noexcept
{
    if (BoundaryLive())
        boundary::Instance().OnTechniquesChanged();
    return false;
}

// Window mode (ext/window): fires BEFORE IDXGISwapChain::SetFullscreenState and returns true to prevent the call. With
// [Window] Mode = off it is one atomic load (and it is not registered at all when the mode is off at load time).
bool WindowSetFullscreenState(swapchain* sc, bool fullscreen, void* hmonitor) noexcept
{
    if (sc == nullptr)
        return false;
    try
    {
        return ext::window::OnSetFullscreenState(sc->get_native(), reinterpret_cast<uint64_t>(sc->get_hwnd()), fullscreen,
                                                 reinterpret_cast<uint64_t>(hmonitor));
    }
    catch (...)
    {
        return false; // fail open: the game's call goes through
    }
}

std::atomic<bool> g_boundaryInstalled { false };
std::atomic<bool> g_windowInstalled { false };
} // namespace

bool InstallWindowEvent()
{
    if (g_windowInstalled.exchange(true))
        return true;
    try
    {
        ::reshade::register_event<::reshade::addon_event::set_fullscreen_state>(&WindowSetFullscreenState);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

probe::IBackend* NewLiveBackend() { return new LiveBackend; }

bool InstallBoundaryTracking()
{
    if (g_boundaryInstalled.exchange(true))
        return true;
    try
    {
        boundary::Instance(); // construct now (DllMain), not at the first draw
        using ::reshade::addon_event;
        ::reshade::register_event<addon_event::init_effect_runtime>(&BoundaryInit);
        ::reshade::register_event<addon_event::destroy_effect_runtime>(&BoundaryDestroy);
        ::reshade::register_event<addon_event::reshade_reloaded_effects>(&BoundaryReloaded);
        ::reshade::register_event<addon_event::reshade_set_current_preset_path>(&BoundaryPresetPath);
        ::reshade::register_event<addon_event::reshade_set_technique_state>(&BoundaryTechniqueState);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

probe::Session* ProbeSession() { return g_session; }

void EnsureReportStarted() { Live(); }

void ProbeAtBoundary(ID3D11DeviceContext* context)
{
    probe::Session* session = g_session;
    if (session == nullptr || !session->Armed() || State::Instance().isShuttingDown)
        return;
    session->ProbeAtBoundary(context);
}
probe::FileReport* ProbeReport() { return g_report; }

bool InstallProbe(const std::filesystem::path& baseFolder, const std::string& registrationLine)
{
    if (g_installed.exchange(true))
        return g_session != nullptr;

    try
    {
        static LiveBackend* backend = new LiveBackend; // never destroyed
        g_report = new probe::FileReport(baseFolder);
        probe::Session* session = new probe::Session(*backend, *g_report);
        g_beginLine = new std::string(registrationLine); // written by the first callback, not here (DllMain)

        // Make the session visible to the callbacks only after it is complete.
        g_session = session;

        using ::reshade::addon_event;
        ::reshade::register_event<addon_event::init_effect_runtime>(&OnInitRuntime);
        ::reshade::register_event<addon_event::destroy_effect_runtime>(&OnDestroyRuntime);
        ::reshade::register_event<addon_event::reshade_reloaded_effects>(&OnReloadedEffects);
        ::reshade::register_event<addon_event::reshade_set_current_preset_path>(&OnPresetPath);
        ::reshade::register_event<addon_event::reshade_begin_effects>(&OnBeginEffects);
        ::reshade::register_event<addon_event::reshade_finish_effects>(&OnFinishEffects);
        ::reshade::register_event<addon_event::reshade_present>(&OnReshadePresent);
        ::reshade::register_event<addon_event::present>(&OnQueuePresent);
        return true;
    }
    catch (...)
    {
        return false;
    }
}
} // namespace ext::reshade
