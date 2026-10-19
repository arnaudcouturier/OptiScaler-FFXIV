#pragma once
// FFXIV game-jitter probe and correction (ext/ffxiv/jitter). One entry point, called from ext::dx11bridge::OnUpscaleBoundary
// (ext/Seams.cpp) on the DirectX 11 bridge's evaluate path, before the parameters go to the upscaler.
//
// [FFXIV] JitterProbe (default off): read-only. Reads the game's camera jitter object with guarded reads and logs what the game applies
//   compared with what it tells NGX (FfxivJitterProbe.h explains the numbers). Never writes game memory.
// [FFXIV] JitterCorrection (default off, experimental): when every guard passes for this evaluate and the game's applied jitter differs
//   from the reported one, the Jitter.Offset.X/Y of THIS evaluate's NGX parameter block are replaced by the applied pixel jitter. Any
//   failed guard passes the game's values through unchanged. The game's camera memory is never written, in either mode; the only
//   write is into the parameter block (the bridge's own NVSDK_NGX_Parameter table), and only with the correction on.
//
// With both keys off the cost is two loads of a config flag and a branch. Never throws.

struct NVSDK_NGX_Parameter;

namespace ext::ffxiv::jitter
{
// `parameters` is the parameter block of the evaluate in progress (the bridge's InParameters, a non-const object): it is read in both
// modes and, with the correction on, Jitter.Offset.X/Y are written into it. The seam declares it non-const for that reason.
void OnUpscaleBoundary(NVSDK_NGX_Parameter* parameters) noexcept;
} // namespace ext::ffxiv::jitter
