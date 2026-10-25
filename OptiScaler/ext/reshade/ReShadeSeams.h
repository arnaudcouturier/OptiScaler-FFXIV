#pragma once
// Seam header of the ReShade built-in add-on (docs/SEAMS-catalog.md group `reshade`), included by ext/Seams.h.
// Deliberately free of ReShade and ImGui headers: upstream files only ever see this declaration.

#include <Windows.h>

// Upstream classes the depth seam names (forward declarations only; the full types come with the host file's own includes)
struct ID3D11DeviceContext;
struct NVSDK_NGX_Parameter;
class IFeature_Dx11;

namespace ext::reshade
{
// dllmain.cpp CheckWorkingMode, right after the `LoadReShade` block. Runs inside DllMain (loader lock): no file or
// folder I/O, no threads, no COM. `reshadeModule` is the module OptiScaler just loaded and may be null (LoadReShade
// off, or ReShade loaded by something else: the process is then searched for a module exporting ReShadeRegisterAddon).
// Registers OptiScaler's own module as a ReShade add-on (API 20) and logs ONE line: "registered (API 20)" or the
// reason it stays inert. Idempotent, never throws, never blocks.
void OnReshadeLoaded(HMODULE reshadeModule);
} // namespace ext::reshade

namespace ext::reshade::depth
{
// Automatic ReShade scene depth (docs/FEATURES.md "Automatic ReShade scene depth"): inputs/NVNGX_DLSS_Dx11.cpp, right after the successful
// deviceContext->Evaluate(...). `context` is the context NGX was given (possibly ReShade's proxy: it is only logged), `feature` the
// evaluating feature (its display size is the Output size of last resort; may be null), `parameters` the NGX parameter block (a null one
// ends the evaluate). Per frame: with [FFXIV] AutomaticReShadeDepth off the cost is ONE relaxed atomic load; with it on in another game it is a few
// relaxed loads and a cached flag. Runs on the NGX evaluate thread. C++ exceptions never leave it. The reads on the pointers the
// upscaler has just used (the parameter block, the Output and Depth resources, the depth's device, the feature) run under SEH, so a
// hardware fault in one of them ends that evaluate; the copy pass guards its own D3D calls. Nothing is read from `parameters` or
// `feature` before the gates have passed.
void OnEvaluated(ID3D11DeviceContext* context, NVSDK_NGX_Parameter* parameters, IFeature_Dx11* feature) noexcept;

// NGX is going away (ext::dx11bridge::OnNgxShutdownBegin and BeforeReleaseFeature): the copy pass gives its resources (and its reference
// to the device) back. On the evaluate thread at once, from any other thread at the next OnEvaluated. A no-op before the first evaluate.
void OnNgxShutdown() noexcept;
} // namespace ext::reshade::depth
