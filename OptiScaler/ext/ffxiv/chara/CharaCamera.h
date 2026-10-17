#pragma once
// Snapshot of the camera constant buffer of the preview's G-buffer pass (pixel-shader slot 2). The helper passes bind the
// copy to slot 1 (CharaGuides.cpp); the motion pass subtracts the last vector's zw from the engine's motion output.
// Rules: CharaCameraRules.h.

#include <d3d11.h>

#include "ext/ffxiv/chara/CharaCameraRules.h"

namespace ext::ffxiv::chara::camera
{
enum class Result : uint8_t
{
    Captured,
    Rejected, // the bound buffer is not a camera block
    Failed,   // the copy could not be made
};

// Copies the buffer bound at pixel-shader slot 2 of the immediate context when it is a camera block (D3D11.1 context
// required). The copy is a private default-usage buffer of the same size.
Result Capture(ID3D11DeviceContext* context);

bool Ready();
ID3D11Buffer* Buffer();
UINT FirstConstant();
UINT ConstantCount();

void Release();
} // namespace ext::ffxiv::chara::camera
