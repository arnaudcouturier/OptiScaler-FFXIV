#pragma once
// Seams of the upscaler-layer group (C1, C2, C3, C5; docs/SEAMS-catalog.md "upscaler"), included by ext/Seams.h.
// Bodies: ext/upscaler/UpscalerSeams.cpp. The pure decision logic (slack math, EffectivePerfQuality's branches,
// the C2/C3 per-axis-min correction) lives in ext/upscaler/ForcedQuality.h and ext/upscaler/RenderGeometry.h so it
// is unit-testable without NVNGX or Config.

#include "ext/upscaler/ForcedQuality.h"

#include <nvsdk_ngx.h>

class IFeature;
struct NVSDK_NGX_Parameter;

namespace ext::upscaler
{
// NVNGX_Parameter.cpp DLSS and DLSSD GetOptimalSettingsCallback: forces DLAA when the game module requires native
// DLAA (ext::game::Current().quality), else applies ForcePerfQuality. `asked` is the game's own PerfQualityValue.
//
// Deliberately does not touch the callback's DRS-maximum block (untouched upstream, no seam there): that window
// has to stay wide enough for whatever the game actually submits, not just the forced preset's own optimal size.
// FFXIV builds a Balanced feature and then submits a full 3840x2160 subrect every frame regardless of the forced
// preset; pinning the maximum to the forced optimal would put the game's real render size outside the legal
// window, and the runtime answers BAD00005 on every frame from then on. The default window (max at display
// resolution) already covers both cases.
NVSDK_NGX_PerfQuality_Value EffectivePerfQuality(NVSDK_NGX_PerfQuality_Value asked);

// Same two call sites, end of the callback: records the answer LastQualityAnswer() reports (menu card,
// ApplyForcedQuality's "does this create descend from the last answer" test).
void OnOptimalSettings(NVSDK_NGX_PerfQuality_Value quality, unsigned int renderWidth, unsigned int renderHeight,
                        unsigned int displayWidth, unsigned int displayHeight);

// The answer OnOptimalSettings last recorded (queries == 0 before the first one).
ForcedQualityStatus LastQualityAnswer();

// IFeature::SetInitParameters: relabels `pq` (and writes it back into `params`) to ForcePerfQuality when this
// create descends from the last answer (32px slack) or nothing has been asked yet; otherwise stands down and
// leaves `pq` untouched. Also ensures the game module's native-quality hooks are installed (lazy, call_once) --
// this is the only call site that does so for the C1/C5 wave, and it runs unconditionally, before any of the
// above checks. Returns whether it relabelled `pq`.
bool ApplyForcedQuality(NVSDK_NGX_Parameter* params, unsigned int width, unsigned int height, unsigned int outWidth,
                         unsigned int outHeight, int& pq);

// IFeature::GetRenderResolution, after the existing subrect-read/fallback block: possibly corrects the render size
// NGX reports for this evaluate (C2/C3, docs/SEAMS.md 5 "RenderSizeFromWidthHeight"). Gated structurally on
// game::Current().geometry != nullptr -- null (every game but a matched FFXIV build) returns false immediately,
// upstream's own subrect-report-only behaviour completely untouched; non-null delegates the config-gated per-axis
// min math to IRenderGeometry::ResolveRenderExtent. `feature` supplies the pre-update render/display size for the
// "Evaluate geometry" diagnostic (logged whenever FFXIV is matched, independent of the gate, matching the oracle's
// unconditional H5c hunk); EnsureHooks() on the native-quality module also runs first and unconditionally here
// (U3 H5a, deferred to this series by C1/C5), through the same cached-pointer path as ApplyForcedQuality. `OutWidth`
// /`OutHeight` are the subrect-derived render size already written by the existing fallback block (in/out).
bool ResolveRenderGeometry(IFeature* feature, const NVSDK_NGX_Parameter* InParameters, unsigned int* OutWidth,
                            unsigned int* OutHeight);

// DLSSFeature.cpp / DLSSDFeature.cpp ProcessEvaluateParams (right after their own GetRenderResolution call),
// IFeature_Dx11wDx12.cpp / IFeature_VkwDx12.cpp Evaluate (DLSS/DLSSD only, right before dx12Feature->Evaluate):
// writes the corrected render size back into DLSS_Render_Subrect_Dimensions_* (the runtime reads the subrect
// itself, not IFeature's own bookkeeping). Free function -- needs ext::Access on IFeature to call the protected
// GetRenderResolution again on an arbitrary feature, so no declaration is added to IFeature.h beyond the friend
// line. Same gate as ResolveRenderGeometry, checked here too before touching anything: a complete no-op (not even
// the extra GetRenderResolution call) when geometry is null or the config flag is off.
void CorrectRenderSubrect(IFeature* feature, NVSDK_NGX_Parameter* InParameters);
} // namespace ext::upscaler
