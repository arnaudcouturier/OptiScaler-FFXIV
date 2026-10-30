#pragma once
// The one call site of the pattern-search resolver in the product (replaces the old compile-check placeholder).
//
// StartOnce(): resolve the main module ONCE, decide which features may run, log one block, publish ActiveBuild() / ActiveGate()
// (ext/ffxiv/ActiveBuild.h). Idempotent and thread-safe (the first caller works, later callers wait and return).
//
// Where it runs, and why there (see ResolveStartup.cpp): the top of ext::game::OnD3D11DeviceHooked (Seams.cpp), the first moment the fork
// is handed the game's D3D11 device. That is on the device-creation thread of the game's startup (not DllMain, so not under the loader
// lock, and before the first frame, so never on a frame-loop render thread), before lighting::OnDeviceHooked and
// before any native-quality / jitter-source Detour over the executable (those install lazily from the upscaler's per-evaluate path).
// Not from Game.cpp's selector: Group 1 must not depend on the resolver, and the legacy path must never construct a LiveImage.
namespace ext::ffxiv::resolve
{
void StartOnce();
}
