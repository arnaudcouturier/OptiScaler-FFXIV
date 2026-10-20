#pragma once
// Tri-state result of ext::dx11bridge::BeforeReleaseFeature and the decision it feeds (the 500 ms sleep before the
// DX11 upscaler is reset). Pure, no upstream types: tests/ext/dx11bridge/release_drain_test.cpp.

#include "ext/gpu/DrainResult.h"

namespace ext::dx11bridge
{
enum class ReleaseDrain
{
    Drained,       // everything the feature could still be running on is proven idle: the sleep adds nothing
    NotApplicable, // no proof (other game, no bridge, queue unknown, device lost, handle not quiet): proceed, keep the sleep
    Failed,        // a required queue could not be drained: the release is refused (PlatformError), as before
};

// Whether an Evaluate of the handle could still be running on another thread.
enum class Quiescence
{
    NotChecked,       // key off or shutting down: nobody was asked
    Idle,             // handle closed to new evaluates, none in flight
    EvaluateInFlight, // the bounded wait timed out
    SelfInside,       // the releasing thread is itself inside an evaluate of this handle
};

// `fgRequired`: FG is attached to this feature (it is the current feature and the FG input is the upscaler), so the
// FG queue has to be proven idle as well as the upscale queue.
inline ReleaseDrain ClassifyRelease(bool fgRequired, gpu::QueueDrain fg, gpu::QueueDrain upscale, Quiescence quiet)
{
    using gpu::QueueDrain;
    if ((fgRequired && fg == QueueDrain::Failed) || upscale == QueueDrain::Failed)
        return ReleaseDrain::Failed;
    if (upscale != QueueDrain::Drained || (fgRequired && fg != QueueDrain::Drained))
        return ReleaseDrain::NotApplicable;
    return quiet == Quiescence::Idle ? ReleaseDrain::Drained : ReleaseDrain::NotApplicable;
}

// True = skip the sleep. Only on a proven drain, only with the key on, never while shutting down (the caller does
// not sleep then anyway).
inline bool SkipReleaseSleep(ReleaseDrain drain, bool keyOn, bool shuttingDown)
{
    return keyOn && !shuttingDown && drain == ReleaseDrain::Drained;
}

inline const char* Name(ReleaseDrain d)
{
    switch (d)
    {
    case ReleaseDrain::Drained:
        return "drained";
    case ReleaseDrain::NotApplicable:
        return "not-applicable";
    case ReleaseDrain::Failed:
        return "failed";
    }
    return "?";
}
} // namespace ext::dx11bridge
