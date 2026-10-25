#pragma once
// Automatic ReShade scene depth: what the six ReShade event callbacks do (ReShadeDepthEvents.cpp has the callbacks themselves, which
// only translate effect_runtime* to an id). No ReShade types here, so the rules are tested with a fake host and the real binder
// (tests/ext/reshade/reshade_depth_events.cpp).
//
// The rules:
//  - Nothing here looks at the option. The binder runs its retry of an unfinished pass or an unrestored binding BEFORE its own enabled
//    check on purpose: a runtime that is dirty, or has a pass open when the user switches the option off, is still repaired. An early
//    return on "disabled" in these callbacks would leave our copy bound (and frozen) in ReShade's map until an effect reload.
//  - The frame counter is depth-owned: one tick per finish_effects of a runtime on our device, whether or not the pass was bound
//    (a counter that only ticked on bound passes would stall exactly when a capture is missing). The capture side stamps its capture
//    with the counter, the binder's Frame() reads it, so both agree within a game frame. State::frameCount counts generated frames too
//    and is not used.
//  - The registry of D3D11 runtimes (the backend's fallback for "the only D3D11 runtime") is fed by begin and finish, and emptied by
//    destroy.

#include "ext/reshade/ReShadeDepthBackend.h"
#include "ext/reshade/ReShadeDepthCore.h"

#include <atomic>
#include <cstdint>

namespace ext::reshade::depth
{
class EventDispatcher
{
  public:
    EventDispatcher(DepthBinder& binder, IEventHost& host, std::atomic<uint64_t>& frame) : _binder(binder), _host(host), _frame(frame) {}

    void Begin(uint64_t runtime) noexcept
    {
        Note(runtime);
        _binder.OnBeginEffects(runtime);
    }

    void Finish(uint64_t runtime) noexcept
    {
        Note(runtime);
        _binder.OnFinishEffects(runtime);
        // After the binder: a finish never reads the frame, and the capture of the next frame is stamped with the new value.
        if (Ours(runtime))
            _frame.fetch_add(1, std::memory_order_relaxed);
    }

    void Destroy(uint64_t runtime) noexcept
    {
        _binder.OnDestroyRuntime(runtime);
        Forget(runtime);
    }

    void Reloaded(uint64_t runtime) noexcept { _binder.OnReloadedEffects(runtime); }
    void TechniqueState(uint64_t runtime) noexcept { _binder.OnTechniquesChanged(runtime); }
    void PresetPath(uint64_t runtime) noexcept { _binder.OnTechniquesChanged(runtime); }

  private:
    void Note(uint64_t runtime) noexcept
    {
        try
        {
            _host.NoteRuntime(runtime);
        }
        catch (...)
        {
        }
    }
    void Forget(uint64_t runtime) noexcept
    {
        try
        {
            _host.ForgetRuntime(runtime);
        }
        catch (...)
        {
        }
    }
    bool Ours(uint64_t runtime) noexcept
    {
        try
        {
            return _host.IsOurRuntime(runtime);
        }
        catch (...)
        {
            return false;
        }
    }

    DepthBinder& _binder;
    IEventHost& _host;
    std::atomic<uint64_t>& _frame;
};
} // namespace ext::reshade::depth
