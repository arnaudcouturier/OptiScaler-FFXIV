#pragma once
// Automatic ReShade scene depth: what the six ReShade event callbacks do (ReShadeDepthEvents.cpp has the callbacks themselves, which
// only translate effect_runtime* to an id). No ReShade types here, so the rules are tested with a fake host and the real binder
// (tests/ext/reshade/reshade_depth_events.cpp).
//
// The rules:
//  - Nothing here looks at the option. The binder runs its retry of an unfinished pass or an unrestored binding BEFORE its own enabled
//    check on purpose: a runtime that is dirty, or has a pass open when the user switches the option off, is still repaired. An early
//    return on "disabled" in these callbacks would leave our copy bound (and frozen) in ReShade's map until an effect reload.
//  - The frame counter is depth-owned and belongs to the CAPTURE side (ReShadeDepthEval.cpp): one tick per capture, before it is published.
//    The binder's Frame() reads it. No event moves it: ReShade runs an effect pass for every OUTPUT frame, so under frame generation it
//    runs several passes between two evaluates; every one of them sees the stamp of the last capture (delta 0) and binds, and a capture
//    that stops being made is caught by its age (kMaxCaptureAgeMs), not by a frame difference. (The first version ticked at every finish
//    and refused 60 % of the passes as stale under frame generation.) State::frameCount counts generated frames too and is not used.
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
    EventDispatcher(DepthBinder& binder, IEventHost& host) : _binder(binder), _host(host) {}

    // `context` = the native immediate context of this begin_effects (the G-buffer remap draws on it), 0 when unknown.
    void Begin(uint64_t runtime, uint64_t context = 0) noexcept
    {
        Note(runtime);
        _binder.OnBeginEffects(runtime, context);
    }

    void Finish(uint64_t runtime) noexcept
    {
        Note(runtime);
        _binder.OnFinishEffects(runtime);
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
    DepthBinder& _binder;
    IEventHost& _host;
};
} // namespace ext::reshade::depth
