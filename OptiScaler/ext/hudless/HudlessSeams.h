#pragma once
// The HUD-less engine's upstream attachment points (docs/HUDLESS.md section 4.3, catalog group `hudless`), included by
// ext/Seams.h. Bodies: ext/hudless/HudlessEngine.cpp. Each is an inline wrapper testing the feature bit ext::F::Hudless
// (set at config load, cleared by the engine once it has seen mode off), so a build with [HudLess] Mode = off pays one
// relaxed load and one branch per call.

#include "ext/ExtFeatures.h"

#include "ext/hudless/PlateSeams.h"
#include "ext/hudless/PlateSource.h"

#include <d3d12.h>

class Dx11wDx12SC;
struct IDXGISwapChain;

namespace ext::hudless
{
namespace detail
{
void PreInteropCopy(Dx11wDx12SC& sc);
void UiFreeFrame(Dx11wDx12SC& sc, unsigned int slot, ID3D12Resource*& copySource, D3D12_RESOURCE_STATES& sourceState);
void PostInteropCopy(Dx11wDx12SC& sc);
void OnReleaseInterop(Dx11wDx12SC& sc);
void BeforeOverlay(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue);
// Developer probes ([HudLess] PlateDiagnostics, PlateProbe.cpp): they take no part in the engine and are reached through their own bit.
void PlateProbePost(Dx11wDx12SC& sc);
void PlateProbeExpected(uint32_t outputsPerFrame);
void PlateProbeOutput(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue);
} // namespace detail

// dx11_with_dx12_sc.cpp Present, after the backbuffer was copied to the shared texture and before _WaitDx11ThenDx12
// signals the D3D11 fence: takes the newest UI-free capture from the game's IHudlessSource and copies it into a free
// slot's shared texture on the D3D11 immediate context, so the one existing interop fence covers both copies.
inline void PreInteropCopy(Dx11wDx12SC& sc)
{
    if (ext::Active(ext::F::Hudless))
        detail::PreInteropCopy(sc);
}

// _CopyDx11SharedToDx12FGBackBuffer, right after copySource is chosen and before ConvertCopySource (so OptiHDR converts
// the swapped image): in ui_free mode, when every guard holds, replaces the frame generator's base (copySource, resting
// state COMMON) with the hud-less image. Nothing changes otherwise. The physical site is inside the copy function
// because that is where the source is picked; logically it is "before the interop copy" of docs/HUDLESS.md 2.
inline void UiFreeFrame(Dx11wDx12SC& sc, unsigned int slot, ID3D12Resource*& copySource,
                        D3D12_RESOURCE_STATES& sourceState)
{
    if (ext::Active(ext::F::Hudless))
        detail::UiFreeFrame(sc, slot, copySource, sourceState);
}

// Present, after _WaitForInteropCopyOnPresentQueue (the FG queue has waited for the interop copy): tags HudlessColor /
// UIColor on the FG UI command list, builds the paste image on the FG queue, counts the present. Also the real-frame point of the developer
// probe and of the native nameplate engine: one test of the three bits when they are all off; a consumer only runs with its own bit
// (the probe records first, before the engine spends its time).
inline void PostInteropCopy(Dx11wDx12SC& sc)
{
    if (ext::ActiveAny(ext::F::Hudless, ext::F::PlateProbe, ext::F::Plate))
    {
        if (ext::Active(ext::F::PlateProbe))
            detail::PlateProbePost(sc);
        if (ext::Active(ext::F::Plate))
            ext::hudless::plates::OnPost(sc);
        if (ext::Active(ext::F::Hudless))
            detail::PostInteropCopy(sc);
    }
}

// _ReleaseInteropBackBuffers (resize, teardown), before the bridge drops its own copies: retires the slot ring behind
// its GPU fences, never blocks. Also the native nameplate engine's own retirement (its slots, record and pins).
inline void OnReleaseInterop(Dx11wDx12SC& sc)
{
    if (ext::Active(ext::F::Plate))
        ext::hudless::plates::OnRelease();
    if (ext::Active(ext::F::Hudless))
        detail::OnReleaseInterop(sc);
}

// fg AdjustDlssgOptions, after the multiplier selector and the menu interlock: how many outputs this real frame was dispatched for
// (generated frames + 1; 0 when it is not known in advance). The developer probe and the native nameplate engine listen.
inline void NoteExpectedMultiplier(uint32_t outputsPerFrame)
{
    if (ext::ActiveAny(ext::F::PlateProbe, ext::F::Plate))
    {
        if (ext::Active(ext::F::Plate))
            ext::hudless::plates::NoteMultiplier(outputsPerFrame);
        if (ext::Active(ext::F::PlateProbe))
            detail::PlateProbeExpected(outputsPerFrame);
    }
}

// The bridge's present scope constructor (dx11_with_dx12_sc.cpp, its Present entry, before the backbuffer copy): the
// frame boundary of the native nameplate engine. One relaxed load with the bit off.
inline void OnPresentBegin()
{
    if (ext::Active(ext::F::Plate))
        ext::hudless::plates::OnPresentBegin();
}
} // namespace ext::hudless

namespace ext::present
{
// wrapped_swapchain.cpp LocalPresent, before MenuOverlayDx::Present: general pre-overlay slot (SEAMS 5). Today's only
// user is the HUD-less engine's UI paste, drawn over every output frame of the frame generation swapchain (real and
// generated). `queue` is the swapchain's command queue (null for a D3D11 swapchain).
//
// The developer probe (PlateDiagnostics) is a second consumer of this hook: it records the output first, with its own bit, and never
// reaches the engine; the engine only runs with its own bit. The native nameplate engine is a third (its output pass draws the plate
// image over every output). With all three bits clear this is one load and one test, as before.
inline void BeforeOverlay(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue, bool isD3D11)
{
    if (!isD3D11 && queue != nullptr && ext::ActiveAny(ext::F::Hudless, ext::F::PlateProbe, ext::F::Plate))
    {
        if (ext::Active(ext::F::PlateProbe))
            hudless::detail::PlateProbeOutput(swapChain, queue);
        if (ext::Active(ext::F::Plate))
            ext::hudless::plates::OnOverlay(swapChain, queue);
        if (ext::Active(ext::F::Hudless))
            hudless::detail::BeforeOverlay(swapChain, queue);
    }
}
} // namespace ext::present
