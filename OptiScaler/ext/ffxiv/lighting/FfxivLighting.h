#pragma once

#include <d3d11.h>

namespace ext::game
{
struct ISceneLighting;
}

namespace ext::ffxiv::lighting
{
ext::game::ISceneLighting& Provider();
void OnDeviceHooked(ID3D11Device* device);
void OnUnhook();
} // namespace ext::ffxiv::lighting
