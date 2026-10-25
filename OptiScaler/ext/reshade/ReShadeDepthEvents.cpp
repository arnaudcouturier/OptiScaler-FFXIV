#include "pch.h"

#include "ext/reshade/ReShadeD3D11.h"
#include "ext/reshade/ReShadeDepthDispatch.h"
#include "ext/reshade/ReShadeDepthEvents.h"

// ReShade's headers and ImGui never share a translation unit (see ReShadeRegister.cpp).
#ifdef IMGUI_VERSION_NUM
#error "ReShadeDepthEvents.cpp must not see imgui.h"
#endif

#include "ext/reshade/third_party/reshade/reshade.hpp"

#include <array>
#include <atomic>
#include <mutex>
#include <string>

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
std::atomic<uint8_t> g_path { static_cast<uint8_t>(SubscribedVia::None) };
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

// ---- the callbacks (noexcept: nothing may escape into ReShade or the game) ---------------------------------------------------------
// The thread a callback runs on, once for init, begin and finish: the load path subscribes from the init callback, i.e. on the thread
// that creates the effect runtime, and if ReShade delivers begin and finish on another one the log shows it (see the runbook).
std::atomic<bool> g_initThreadLogged { false };
// begin_effects: once per THREAD (the first four), with the evaluate thread next to it
std::atomic<uint32_t> g_beginThreads[4] {};
std::atomic<ThreadIdFn> g_evaluateThread { nullptr };
std::atomic<bool> g_finishThreadLogged { false };

void LogThreadOnce(std::atomic<bool>& flag, [[maybe_unused]] const char* what)
{
    if (!flag.load(std::memory_order_relaxed) && !flag.exchange(true, std::memory_order_relaxed))
        LOG_INFO("Automatic ReShade depth: first {} callback on thread {}", what, GetCurrentThreadId());
}

void SubscribeRuntimeEvents(uint64_t runtime) noexcept;

void LogBeginThreadOnce()
{
    const uint32_t thread = GetCurrentThreadId();
    for (std::atomic<uint32_t>& slot : g_beginThreads)
    {
        uint32_t seen = slot.load(std::memory_order_relaxed);
        if (seen == 0 && slot.compare_exchange_strong(seen, thread))
        {
            [[maybe_unused]] const ThreadIdFn source = g_evaluateThread.load(std::memory_order_acquire);
            [[maybe_unused]] const uint32_t evaluate = source != nullptr ? source() : 0;
            LOG_INFO("Automatic ReShade depth: first begin_effects callback on thread {} (evaluate thread {}{})", thread,
                     evaluate != 0 ? std::to_string(evaluate) : std::string("not known yet"),
                     evaluate == 0 ? "" : (evaluate == thread ? ", the same thread" : ", another thread"));
            return;
        }
        if (seen == thread)
            return;
    }
}

// The init callback subscribes finish and begin (load path). It looks at neither the dispatcher nor the option: the facade does not exist
// until the first evaluate with the option on, and the runtime is created long before that.
void DepthOnInit(effect_runtime* runtime) noexcept
{
    if (State::Instance().isShuttingDown)
        return;
    LogThreadOnce(g_initThreadLogged, "init_effect_runtime");
    SubscribeRuntimeEvents(Id(runtime));
}

// The native immediate context of this begin_effects (for D3D11 command_list::get_native() is the ID3D11DeviceContext*). The binder only
// uses it after it found the runtime to be a D3D11 one, and the remap refuses a deferred context. A fault is "no context".
struct NativeJob
{
    command_list* list;
    uint64_t native;
};
void NativeThunk(void* p)
{
    auto& j = *static_cast<NativeJob*>(p);
    j.native = j.list != nullptr ? j.list->get_native() : 0;
}
uint64_t NativeContext(command_list* list) noexcept
{
    NativeJob job { list, 0 };
    try
    {
        return d3d11::GuardedCall(&NativeThunk, &job) == 0 ? job.native : 0;
    }
    catch (...)
    {
        return 0;
    }
}

void DepthOnBegin(effect_runtime* runtime, command_list* list, resource_view, resource_view) noexcept
{
    if (EventDispatcher* d = Live())
    {
        LogBeginThreadOnce();
        d->Begin(Id(runtime), NativeContext(list));
    }
}

void DepthOnFinish(effect_runtime* runtime, command_list*, resource_view, resource_view) noexcept
{
    if (EventDispatcher* d = Live())
    {
        LogThreadOnce(g_finishThreadLogged, "finish_effects");
        d->Finish(Id(runtime));
    }
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

// A POD thunk, so that SEH can guard the call into ReShade (register_event resolves ReShade's entry point on its first use per event
// and calls it): a fault inside it is a failed subscription, not a crash.
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
    case DepthEvent::InitEffectRuntime:
        ::reshade::register_event<addon_event::init_effect_runtime>(&DepthOnInit);
        break;
    case DepthEvent::Count:
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

// Subscribes one event unless it is done or failed once (a failure is remembered and not tried again). Begin is held back, and marked
// failed with the others, when any other event could not be subscribed: a begin without its finish and its destroy would leave our
// copy in ReShade's map. The caller holds g_subscribeMutex.
void SubscribeOne(DepthEvent event)
{
    const uint32_t bit = EventBit(event);
    if (((g_subscribed.load() | g_failed.load()) & bit) != 0)
        return; // done, or failed once and not tried again
    if (event == DepthEvent::BeginEffects && (g_failed.load() & ~bit) != 0)
    {
        g_failed.fetch_or(bit);
        LOG_WARN("Automatic ReShade depth: not subscribing {} because another event could not be subscribed", DepthEventName(event));
        return;
    }
    if (Subscribe(event))
        g_subscribed.fetch_or(bit);
    else
    {
        g_failed.fetch_or(bit);
        LOG_WARN("Automatic ReShade depth: could not subscribe to {}", DepthEventName(event));
    }
}

// finish, then begin: from the init_effect_runtime callback (load path). Idempotent across runtimes (ResizeBuffers makes a new one).
void SubscribeRuntimeEvents([[maybe_unused]] uint64_t runtime) noexcept
{
    // nothing to do once both are subscribed or failed: the common case after the first runtime, without the mutex
    if (((g_subscribed.load(std::memory_order_acquire) | g_failed.load(std::memory_order_acquire)) & kRuntimeDepthEvents) == kRuntimeDepthEvents)
        return;
    try
    {
        std::lock_guard lock(g_subscribeMutex);
        const uint32_t before = g_subscribed.load();
        for (const DepthEvent event : LateRegistrationPlanner::kRuntimeEvents)
            SubscribeOne(event);
        if ((g_subscribed.load() & kRuntimeDepthEvents) == kRuntimeDepthEvents && (before & kRuntimeDepthEvents) != kRuntimeDepthEvents)
            LOG_INFO("Automatic ReShade depth: subscribed to finish_effects and begin_effects from init_effect_runtime (effect runtime {:#x}), "
                     "after the begin_effects of the add-ons that registered theirs at device creation, on thread {}",
                     runtime, GetCurrentThreadId());
    }
    catch (...)
    {
    }
}
} // namespace

void SetDispatcher(EventDispatcher* dispatcher) noexcept { g_dispatcher.store(dispatcher, std::memory_order_release); }

void SetEvaluateThreadSource(ThreadIdFn source) noexcept { g_evaluateThread.store(source, std::memory_order_release); }

uint32_t SubscribedEvents() noexcept { return g_subscribed.load(std::memory_order_acquire); }
uint32_t FailedEvents() noexcept { return g_failed.load(std::memory_order_acquire); }
SubscribedVia SubscriptionPath() noexcept { return static_cast<SubscribedVia>(g_path.load(std::memory_order_acquire)); }

bool EnsureSubscribed(SubscribeWhen when)
{
    try
    {
        std::lock_guard lock(g_subscribeMutex);
        // The first call decides the path; the other kind of call is then a no-op (it only reports).
        uint8_t none = static_cast<uint8_t>(SubscribedVia::None);
        g_path.compare_exchange_strong(none, static_cast<uint8_t>(when == SubscribeWhen::Load ? SubscribedVia::Load : SubscribedVia::Late));
        const bool load = SubscriptionPath() == SubscribedVia::Load;
        const uint32_t needed = load ? kLoadDepthEvents : kLateDepthEvents;
        const uint32_t before = g_subscribed.load();
        if ((before & needed) == needed)
            return true;
        if (load)
        {
            // init_effect_runtime is last (kLoadEvents): the other four exist when the first runtime appears. Begin and finish wait for it.
            for (const DepthEvent event : LateRegistrationPlanner::kLoadEvents)
                SubscribeOne(event);
            if ((g_subscribed.load() & needed) == needed)
                LOG_INFO("Automatic ReShade depth: subscribed to destroy_effect_runtime, reshade_reloaded_effects, reshade_set_technique_state, "
                         "reshade_set_current_preset_path and init_effect_runtime at load (begin_effects and finish_effects follow from the first "
                         "init_effect_runtime, after the add-ons that registered theirs at device creation), on thread {}",
                         GetCurrentThreadId());
        }
        else
        {
            // begin last: a begin without its finish would leave our copy bound until the next begin
            for (const DepthEvent event : LateRegistrationPlanner::kEvents)
                if (event != DepthEvent::BeginEffects)
                    SubscribeOne(event);
            SubscribeOne(DepthEvent::BeginEffects);
            if ((g_subscribed.load() & needed) == needed)
                LOG_INFO("Automatic ReShade depth: subscribed to begin_effects, finish_effects, destroy_effect_runtime, reshade_reloaded_effects, "
                         "reshade_set_technique_state and reshade_set_current_preset_path, late (ReShade loaded after OptiScaler; its dispatch "
                         "may be running on another thread), on thread {}",
                         GetCurrentThreadId());
        }
        return (g_subscribed.load() & needed) == needed;
    }
    catch (...)
    {
    }
    return false;
}
} // namespace ext::reshade::depth
