#pragma once
// Automatic ReShade scene depth: the facade the rest of the fork sees (the NGX seam, the menu card, the config). No ReShade, NGX or
// ImGui headers here. The pieces behind it: the copy pass (ReShadeDepthCopy.h), the binder (ReShadeDepthCore.h) over the live backend
// (ReShadeDepthBackend.h), the six ReShade event callbacks (ReShadeDepthEvents.h), the late registration (ReShadeDepthLate.h) and the
// pure glue decisions (ReShadeDepthGlue.h).
//
// The singletons (binder, backend, capture engine, copy pass) are heap-allocated and never destroyed: nothing runs at DLL_PROCESS_DETACH,
// and ReShade may still deliver events while the process shuts down. They are built by the first OnEvaluated that finds the option on
// (the evaluate thread, outside DllMain): config load and reload only STORE the option (SetEnabled), they construct nothing. The binder's
// Shutdown() is never called while a ReShade runtime may be alive: switching the option off is SetEnabled(false), which lets
// finish_effects put the binding back.
//
// Threading: Enabled/SetEnabled/Status/GetCounters/GetCaptureStats/GetConsumer are callable from any thread. OnEvaluated (declared in
// ReShadeSeams.h, the one declaration the seam and this file share) runs on the NGX evaluate thread, which owns the copy pass and its
// context; whoever else wants something done to the pass (the option was switched off, NGX ends) leaves a request that the next
// OnEvaluated carries out.

#include "ext/reshade/ReShadeDepthCore.h"

#include <cstdint>
#include <string>

namespace ext::reshade::depth
{
// The option ([FFXIV] AutomaticReShadeDepth). ONE relaxed atomic load: the disabled seam costs this and nothing else. SetEnabled keeps
// it, the binder (once it exists) and the request flags in step: it is called at config load and reload (ExtConfig::PostLoad, through
// FfxivConfig::automaticReShadeDepthOnLoaded) and from the menu toggle, and does nothing when the value does not change. Switching off
// asks the evaluate thread to withdraw the capture and release the pass; before the first evaluate there is nothing to ask.
bool Enabled() noexcept;
void SetEnabled(bool on) noexcept;

// The status line of the menu card: "Automatic scene depth: 1920x1080 -> 3840x2160" while a pass is bound, otherwise why not
// ("ReShade not detected", "Waiting for ReShade depth effects", ...). Reflects the stored option before the first evaluate.
std::string Status();

Counters GetCounters();

// What the capture side did (the evaluate thread writes, any thread reads).
struct CaptureStats
{
    uint64_t captured = 0;      // captures published
    uint64_t refused = 0;       // requests the pass refused or failed
    uint32_t lastResult = 0;    // depth::Result of the last capture, 0 = Ok
    uint64_t frame = 0;         // the depth-owned frame counter
};
CaptureStats GetCaptureStats();

// Whether an effect runtime has asked for depth lately, and the size of its back buffer (the size of the copy).
ConsumerInfo GetConsumer(int64_t nowMs);

// (OnEvaluated and OnNgxShutdown, the seams, are declared in ReShadeSeams.h.)
} // namespace ext::reshade::depth
