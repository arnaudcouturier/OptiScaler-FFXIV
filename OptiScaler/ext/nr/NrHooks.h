#pragma once
// Hooks that NR calls into code owned by later series (docs/SEAMS.md section 2: feature code never names the HDR or
// FFXIV modules). With nothing registered NR behaves exactly like the reference build with every one of these features
// switched off, which is also the default of the reference build (HDR.NRSceneInput, HDR.DLSSSceneInput, FfxivHDR).
//
//   * Scene HDR input (C10, series S6): a same-frame pre-clipping scene image that NR (A1/B1) can run on instead of
//     the SDR colour, and the reconstructed display image that the private PreSR upscale publishes. ext/hdr fills
//     ext::nr::SetSceneHdrHooks() at init. Image/packet types stay opaque here (Hdr10::SceneInput, NrSceneInput::Packet).
//   * FFXIV bridge behaviours (S7): ext::game::Current().nrQuirks and .lighting, see ext/game/GameInterfaces.h.

#include "ext/game/Game.h"

#include <d3d12.h>
#include <memory>

namespace ext::gpu
{
class GpuLifetime;
}

namespace ext::nr
{
// Same enumerators and order as Hdr10::NrSceneState (C10); the hook implementation converts.
enum class SceneState
{
    Off,
    Ordinary,
    PreSR,
    Alternating,
    WaitingScene,
    WaitingGpu,
    WaitingNR,
    UnsupportedFormat,
    AllocationFailed,
    UnavailableHooks,
    RequiresBeforeSR,
    HDRInactive,
    NRDisabled
};

// Opaque same-frame HDR scene image (C10: Hdr10::SceneInput). Copyable; empty = none.
struct SceneImage
{
    std::shared_ptr<void> impl;
    explicit operator bool() const { return impl != nullptr; }
};

// Opaque prepared scene packet of one NR dispatch (C10: NrSceneInput::Packet*). `work` is the HDR-converted colour
// NR runs on; null token = SDR fallback.
struct ScenePacket
{
    void* token = nullptr;
    ID3D12Resource* work = nullptr;
    explicit operator bool() const { return token != nullptr; }
};

// Every pointer may be null (= feature off).
struct SceneHdrHooks
{
    // --- HDR.NRSceneInput ---------------------------------------------------------------------------------------
    // SceneNrRequested(): NRSceneInput enabled, scene HDR requested and the FFXIV bridge game.
    bool (*nrRequested)() = nullptr;
    // FfxivSceneHdr::Open(device, width, height, display); empty when the scene has no HDR image (scene.hdr null)
    SceneImage (*open)(ID3D12Device*, UINT width, UINT height, bool display) = nullptr;
    // scene.hdr && scene.reference
    bool (*usable)(const SceneImage&) = nullptr;
    // NrSceneInput::Prepare; `failure` receives the reason when the result is empty
    ScenePacket (*prepare)(ID3D12Device*, ID3D12GraphicsCommandList*, ID3D12Resource* native, D3D12_RESOURCE_STATES arrival,
                           UINT width, UINT height, SceneImage&& scene, gpu::GpuLifetime& lifetime,
                           SceneState* failure) = nullptr;
    void (*finish)(ScenePacket&, ID3D12GraphicsCommandList*, D3D12_RESOURCE_STATES arrival) = nullptr;
    // NrSceneInput::Collect(keepWarm)
    void (*collect)(bool keepWarm) = nullptr;
    // Hdr10::nrSceneState store (menu status)
    void (*setState)(SceneState) = nullptr;
    // Start of every NR evaluate: derives Off / NRDisabled / HDRInactive / RequiresBeforeSR / WaitingNR from the
    // HDR.NRSceneInput switch, SceneNrRequested() and the two arguments (the cascade at the top of EvaluateInternal)
    void (*updateState)(bool nrEnabled, bool preSrPlacement) = nullptr;

    // --- HDR.DLSSSceneInput (private PreSR display carrier) -----------------------------------------------------
    // cfg.FfxivHDRDLSSSceneInput && FfxivSceneHdr::Requested()
    bool (*dlssRequested)() = nullptr;
    // displayHdr.referenceHdrShoulder
    bool (*hdrShoulder)(const SceneImage&) = nullptr;
    // DlssSceneInput::Reconcile; true -> publish
    bool (*reconcile)(SceneImage&, ID3D12GraphicsCommandList*, ID3D12Resource* output, D3D12_RESOURCE_STATES arrival) = nullptr;
    // FfxivSceneHdr::PublishReconstructed
    void (*publish)(SceneImage&&) = nullptr;
};

void SetSceneHdrHooks(const SceneHdrHooks& hooks); // ext/hdr, once at init
const SceneHdrHooks& SceneHdr();

// --- Game behaviours (ext::game::Current(); all false/null for any game but a matched FFXIV build) ---------------
// NR behaviours that follow the FFXIV DX11 bridge path (history reset when NR is switched off, split frame work,
// motion-onset sampling). Was `State::gameExe == "ffxiv_dx11.exe"`.
inline bool BridgedNativeTitle()
{
    auto* quirks = ext::game::Current().nrQuirks;
    return quirks != nullptr && quirks->BridgedNativeDx11();
}
// Native scanned lighting events (B6): null for every other game.
inline ext::game::ISceneLighting* NativeLighting() { return ext::game::Current().lighting; }
} // namespace ext::nr
