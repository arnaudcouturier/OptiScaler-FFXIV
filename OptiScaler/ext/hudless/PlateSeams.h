#pragma once
// The native nameplate engine's three present-seam entry points, declared without the engine's own headers: ext/Seams.h
// is included by every upstream file that has a seam, so it must not pull in the plate slot table and its rules.
// Bodies: ext/hudless/UiPlates.cpp.

class Dx11wDx12SC;
struct ID3D12CommandQueue;
struct IDXGISwapChain;

namespace ext::hudless::plates
{
// Present entry, before the backbuffer copy and before any early return of the bridge (the D3D11 bridge's present scope):
// the frame boundary. A reservation left over from an earlier present is dropped here, so a present that never reached
// the frame finish cannot lend its slot and its "hidden" answer to the next frame.
void OnPresentBegin();
// After the interop copy on the game's render thread: finish the frame, publish one record, signal and promote it.
void OnPost(Dx11wDx12SC& sc);
// Resize, teardown, device change: slots retired behind their fences, record cleared, pins dropped.
void OnRelease();
// Every output, before the HUD paste: draw the newest hidden record's plate image.
void OnOverlay(IDXGISwapChain* swapChain, ID3D12CommandQueue* presentQueue);
} // namespace ext::hudless::plates
