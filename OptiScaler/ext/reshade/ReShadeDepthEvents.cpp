#include "pch.h"

#include "ext/reshade/ReShadeD3D11.h"
#include "ext/reshade/ReShadeDepthDispatch.h"
#include "ext/reshade/ReShadeDepthEvents.h"

// ReShade's headers and ImGui never share a translation unit (see ReShadeRegister.cpp).
#ifdef IMGUI_VERSION_NUM
#error "ReShadeDepthEvents.cpp must not see imgui.h"
#endif

#include "ext/reshade/third_party/reshade/reshade.hpp"

#include <atomic>
#include <mutex>

namespace ext::reshade::depth
{
namespace
{
using ::reshade::addon_event;
using ::reshade::api::command_list;
using ::reshade::api::effect_runtime;
using ::reshade::api::effect_technique;
using ::reshade::api::resource_view;

std::atomic<EventDispatcher*> g_dispatcher { nullptr };
std::atomic<uint32_t> g_subscribed { 0 };
std::atomic<uint32_t> g_failed { 0 };
std::mutex g_subscribeMutex;

uint64_t Id(effect_runtime* runtime) { return reinterpret_cast<uint64_t>(runtime); }

// Callbacks may still arrive while the process shuts down: upstream sets isShuttingDown before it frees ReShade. Nothing else is
// checked: the callbacks must not gate on the option (see ReShadeDepthDispatch.h).
EventDispatcher* Live()
{
    if (State::Instance().isShuttingDown)
        return nullptr;
    return g_dispatcher.load(std::memory_order_acquire);
}

// ---- the six callbacks (noexcept: nothing may escape into ReShade or the game) -----------------------------------------------------
void DepthOnBegin(effect_runtime* runtime, command_list*, resource_view, resource_view) noexcept
{
    if (EventDispatcher* d = Live())
        d->Begin(Id(runtime));
}

void DepthOnFinish(effect_runtime* runtime, command_list*, resource_view, resource_view) noexcept
{
    if (EventDispatcher* d = Live())
        d->Finish(Id(runtime));
}

void DepthOnDestroy(effect_runtime* runtime) noexcept
{
    if (EventDispatcher* d = Live())
        d->Destroy(Id(runtime));
}

void DepthOnReloaded(effect_runtime* runtime) noexcept
{
    if (EventDispatcher* d = Live())
        d->Reloaded(Id(runtime));
}

// Fires BEFORE the change and returns true to veto it: this one never vetoes, the next begin recomputes (and the preset-path event,
// which fires after, clears again).
bool DepthOnTechniqueState(effect_runtime* runtime, effect_technique, bool) noexcept
{
    if (EventDispatcher* d = Live())
        d->TechniqueState(Id(runtime));
    return false;
}

void DepthOnPresetPath(effect_runtime* runtime, const char*) noexcept
{
    if (EventDispatcher* d = Live())
        d->PresetPath(Id(runtime));
}

// ---- subscription ----------------------------------------------------------------------------------------------------------------
struct SubscribeJob
{
    DepthEvent event;
};

// A POD thunk: ReShade's registration is the first call into the add-on API after the module scan, and the callbacks are ours.
void SubscribeThunk(void* p)
{
    switch (static_cast<SubscribeJob*>(p)->event)
    {
    case DepthEvent::BeginEffects:
        ::reshade::register_event<addon_event::reshade_begin_effects>(&DepthOnBegin);
        break;
    case DepthEvent::FinishEffects:
        ::reshade::register_event<addon_event::reshade_finish_effects>(&DepthOnFinish);
        break;
    case DepthEvent::DestroyEffectRuntime:
        ::reshade::register_event<addon_event::destroy_effect_runtime>(&DepthOnDestroy);
        break;
    case DepthEvent::ReloadedEffects:
        ::reshade::register_event<addon_event::reshade_reloaded_effects>(&DepthOnReloaded);
        break;
    case DepthEvent::TechniqueState:
        ::reshade::register_event<addon_event::reshade_set_technique_state>(&DepthOnTechniqueState);
        break;
    case DepthEvent::PresetPath:
        ::reshade::register_event<addon_event::reshade_set_current_preset_path>(&DepthOnPresetPath);
        break;
    }
}

bool Subscribe(DepthEvent event)
{
    SubscribeJob job { event };
    try
    {
        return d3d11::GuardedCall(&SubscribeThunk, &job) == 0;
    }
    catch (...)
    {
        return false;
    }
}

constexpr uint32_t Bit(DepthEvent event) { return 1u << static_cast<uint32_t>(event); }
} // namespace

void SetDispatcher(EventDispatcher* dispatcher) noexcept { g_dispatcher.store(dispatcher, std::memory_order_release); }

uint32_t SubscribedEvents() noexcept { return g_subscribed.load(std::memory_order_acquire); }
uint32_t FailedEvents() noexcept { return g_failed.load(std::memory_order_acquire); }

bool EnsureSubscribed()
{
    if (g_subscribed.load(std::memory_order_acquire) == kAllDepthEvents)
        return true;
    try
    {
        std::lock_guard lock(g_subscribeMutex);
        // begin last: a begin without its finish would leave our copy bound until the next begin
        static constexpr DepthEvent kOrder[] = { DepthEvent::FinishEffects, DepthEvent::DestroyEffectRuntime, DepthEvent::ReloadedEffects,
                                                 DepthEvent::TechniqueState, DepthEvent::PresetPath,         DepthEvent::BeginEffects };
        const uint32_t before = g_subscribed.load();
        for (const DepthEvent event : kOrder)
        {
            const uint32_t bit = Bit(event);
            if (((g_subscribed.load() | g_failed.load()) & bit) != 0)
                continue; // done, or failed once and not tried again
            if (event == DepthEvent::BeginEffects && (g_failed.load() & ~bit) != 0)
            {
                g_failed.fetch_or(bit);
                LOG_WARN("Automatic ReShade depth: not subscribing {} because another event could not be subscribed", DepthEventName(event));
                continue;
            }
            if (Subscribe(event))
                g_subscribed.fetch_or(bit);
            else
            {
                g_failed.fetch_or(bit);
                LOG_WARN("Automatic ReShade depth: could not subscribe to {}", DepthEventName(event));
            }
        }
        if (g_subscribed.load() == kAllDepthEvents && before != kAllDepthEvents)
            LOG_INFO("Automatic ReShade depth: subscribed to begin_effects, finish_effects, destroy_effect_runtime, reshade_reloaded_effects, "
                     "reshade_set_technique_state and reshade_set_current_preset_path");
    }
    catch (...)
    {
    }
    return g_subscribed.load(std::memory_order_acquire) == kAllDepthEvents;
}
} // namespace ext::reshade::depth
