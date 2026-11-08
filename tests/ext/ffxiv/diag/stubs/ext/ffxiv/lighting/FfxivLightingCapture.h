#pragma once
// Stand-in for the lighting capture's hook header when a test builds the draw diagnostic's runtime alone. The diagnostic uses nothing
// from it but the identity of the game's immediate context: it compares the context a draw came from with that one, and never touches
// the capture's own hooks.
#include <d3d11.h>

namespace ext::ffxiv::lighting::FfxivLightingCapture
{
inline ID3D11DeviceContext* context = nullptr; // the game's immediate context, as the draw hooks publish it
}
