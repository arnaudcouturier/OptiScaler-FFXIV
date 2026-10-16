#include "pch.h"

#include "ext/reshade/ReShadeEvents.h"

// ReShade's headers and ImGui never share a translation unit (see ReShadeRegister.cpp).
#ifdef IMGUI_VERSION_NUM
#error "ReShadeEvents.cpp must not see imgui.h"
#endif

#include "ext/reshade/third_party/reshade/reshade.hpp"

#include <atomic>
#include <format>

namespace ext::reshade
{
namespace
{
using namespace ::reshade::api;
using probe::EffectsEvent;

probe::Session* g_session = nullptr; // set once by InstallProbe, never freed
probe::FileReport* g_report = nullptr;
std::atomic<bool> g_installed { false };

// Callbacks may still arrive while the process shuts down: upstream sets isShuttingDown before it frees ReShade.
bool Live() { return g_session != nullptr && !State::Instance().isShuttingDown; }

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

void OnQueuePresent(command_queue*, swapchain*, const rect*, const rect*, uint32_t, const rect*) noexcept
{
    if (Live())
        g_session->OnQueuePresent();
}
} // namespace

probe::Session* ProbeSession() { return g_session; }
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
        session->Begin(registrationLine);

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
