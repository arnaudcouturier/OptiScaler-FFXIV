#pragma once
// Automatic ReShade scene depth: the facade the rest of the fork sees (the NGX seam, the menu card, the config). No ReShade, NGX or
// ImGui headers here. The pieces behind it: the copy pass (ReShadeDepthCopy.h), the binder (ReShadeDepthCore.h) over the live backend
// (ReShadeDepthBackend.h), the six ReShade event callbacks (ReShadeDepthEvents.h), the late registration (ReShadeDepthLate.h) and the
// pure glue decisions (ReShadeDepthGlue.h).
//
// The singletons (binder, backend, copy pass) are heap-allocated and never destroyed: nothing runs at DLL_PROCESS_DETACH, and ReShade
// may still deliver events while the process shuts down. The binder's Shutdown() is never called while a ReShade runtime may be
// alive: switching the option off is SetEnabled(false), which lets finish_effects put the binding back.
//
// Threading: Enabled/SetEnabled/Status/GetCounters/GetCaptureStats/GetConsumer are callable from any thread. OnEvaluated runs on the NGX
// evaluate thread, which owns the copy pass and its context; whoever else wants something done to the pass (the option was switched
// off, NGX ends) leaves a request that the next OnEvaluated carries out.

#include "ext/reshade/ReShadeDepthCore.h"

#include <cstdint>
#include <string>

struct ID3D11DeviceContext;
struct NVSDK_NGX_Parameter;

namespace ext::reshade::depth
{
// The option ([FFXIV] AutomaticReShadeDepth). ONE relaxed atomic load: the disabled seam costs this and nothing else. SetEnabled keeps
// it, the binder and the request flags in step: call it at config load and reload and from the menu toggle (it does nothing when the
// value does not change). Switching off asks the evaluate thread to withdraw the capture and release the pass.
bool Enabled() noexcept;
void SetEnabled(bool on) noexcept;

// The status line of the menu card: "Automatic scene depth: 1920x1080 -> 3840x2160" while a pass is bound, otherwise why not
// ("ReShade not detected", "Waiting for ReShade depth effects", ...).
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

// The capture seam: after a successful D3D11 NGX evaluate (inputs/NVNGX_DLSS_Dx11.cpp), with the context NGX was given (possibly
// ReShade's proxy: only logged) and the parameters. Never throws, never lets an access violation out of the pass. The first thing it
// does is the one-load early-out.
void OnEvaluated(ID3D11DeviceContext* context, NVSDK_NGX_Parameter* parameters) noexcept;

// NGX is going away (ext::dx11bridge::OnNgxShutdownBegin / BeforeReleaseFeature): the pass gives its resources back. On the evaluate
// thread this happens at once, from any other thread it is left to the next OnEvaluated.
void OnNgxShutdown() noexcept;
} // namespace ext::reshade::depth
