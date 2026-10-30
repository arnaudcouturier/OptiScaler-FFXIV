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

// The camera block the pass is drawing with, asked from the binding alone: the constant buffer object bound at the draw's own stage (`stage`),
// its first bound constant and the pass size (camera::BlockKey). Nothing is read from video memory, so this never waits for the GPU. An
// invalid key when nothing that can hold the camera block is bound at that stage.
BlockKey BoundBlock(ID3D11DeviceContext* context, Stage stage, uint32_t width, uint32_t height);

bool Ready();
ID3D11Buffer* Buffer();
UINT FirstConstant();
UINT ConstantCount();

void Release();
} // namespace ext::ffxiv::chara::camera
