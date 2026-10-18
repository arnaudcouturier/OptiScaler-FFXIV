#pragma once
// Spatial anti-aliasing (and optional 2x upscaling) of the equipment preview image. The draw that shows the preview in the
// UI samples its image through t0; this module filters that image first and swaps the filtered one in for the draw.

#include "ext/ffxiv/chara/CharaDraw.h"

namespace ext::ffxiv::chara::enhance
{
// Handles one draw of the equipment display shader (the caller checked: immediate context, api Draw, the shader's role,
// [FFXIV] PreviewAA on, PreviewDLSS off). Returns true when it ran the draw (it called `draw` exactly once), false when
// the draw is left to the caller: the t0 image is not the preview renderer's equipment output, its format or size is not
// supported, or the filter pass could not run.
bool Around(const DrawView& view, DrawCall draw);

// The texture that holds the result of the last spatial AA pass (1x or 2x), or null when the pass has produced nothing. Borrowed:
// valid until the next pass or Detach, draw thread only. Used by the preview image dump.
ID3D11Texture2D* OutputTexture();

// Releases the GPU objects.
void Detach();
} // namespace ext::ffxiv::chara::enhance
