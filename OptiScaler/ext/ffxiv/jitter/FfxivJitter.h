#pragma once
// FFXIV game-jitter probe and correction (ext/ffxiv/jitter). One entry point, called from ext::dx11bridge::OnUpscaleBoundary
// (ext/Seams.cpp) on the DirectX 11 bridge's evaluate path, before the parameters go to the upscaler.
//
// [FFXIV] JitterProbe (default off): read-only. Reads the game's camera jitter object with guarded reads and logs what the game applies
//   compared with what it tells NGX (FfxivJitterProbe.h explains the numbers). Never writes game memory.
// [FFXIV] JitterCorrection (default off, experimental): when every guard passes for this evaluate and the game's applied jitter differs
//   from the reported one, the Jitter.Offset.X/Y of THIS evaluate's NGX parameter block are replaced by the applied pixel jitter. Any
//   failed guard passes the game's values through unchanged. Game memory is never written either way.
//
// With both keys off the cost is two loads of a config flag and a branch. Never throws.

struct NVSDK_NGX_Parameter;

namespace ext::ffxiv::jitter
{
// `parameters` is the game's parameter block of the evaluate in progress (the bridge's InParameters, a non-const object that the seam
// declares const; the correction writes Jitter.Offset.X/Y into it, which is why this file documents the cast).
void OnUpscaleBoundary(const NVSDK_NGX_Parameter* parameters) noexcept;
} // namespace ext::ffxiv::jitter
