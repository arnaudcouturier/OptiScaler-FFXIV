#pragma once
// S10 package A: the D1 snapshot path (docs/HUDLESS.md section 12), armed by the menu button "Save UI diagnostics snapshot".
//
// One press arms the next frame that has a boundary candidate (within 60 frames). In that frame the bound render target is
// copied into a reused staging texture at the R1 candidate and at the R2 candidate (before the candidate draw runs, in
// command order), and at that frame's Present entry the backbuffer is copied the same way ("bridge-copy frame": the bridge
// copies the same backbuffer a moment later; that equivalence is an inference). The staging textures are read back over
// the next frames without waiting and written by the diagnostics writer thread (PNG for 8-bit formats, a raw container
// otherwise). A second press after toggling the game's hide-UI key gives the UI-hidden / UI-shown pair.
//
// Nothing here issues a draw; copies go through the lighting hook's un-hooked trampoline, so FfxivLightingScan and the
// scene-HDR alias state never see them. At most kMaxPresses presses per run, bounded by the image byte budget.

#include "ext/ffxiv/ui/UiBoundaryCore.h"

namespace ext::ffxiv::ui::snapshot
{
inline constexpr uint32_t kMaxPresses = 4;

// Menu thread: asks for a snapshot (ignored when the budget of presses is used up).
void Request();

// Hot path test (one relaxed atomic load): a capture window is open.
bool Armed();

// Render thread, Present entry of the frame `core.Frame()`, before core.FreezeFrame(): arms a pending request, copies the
// backbuffer when this frame had a capture, pumps the readbacks.
void OnPresentEntry(Core& core);

// Render thread, from the draw hook, only when Armed() and the draw changed a candidate (before the draw runs).
void OnCandidate(ID3D11DeviceContext* context, Core& core, const Core::DrawResult& draw);
} // namespace ext::ffxiv::ui::snapshot
