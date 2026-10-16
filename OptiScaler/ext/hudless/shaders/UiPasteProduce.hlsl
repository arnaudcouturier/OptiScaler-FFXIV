// S10 paste image producer (docs/HUDLESS.md section 7). One dispatch per real frame:
//   isUi                                  -> (colour, 1.0)
//   cleanup && previous alpha > 0.75      -> (colour, 0.5)   clears ghosts of UI that just closed
//   otherwise                             -> (0, 0, 0, 0)
// colour = FinalTexture, or HdrFinalTexture (PQ) when UseHdr != 0 (the mask is always computed on the SDR pair).
// The number of isUi pixels is added to Coverage[0] (one global atomic per group); the CPU zeroes it before the dispatch.
// Cleanup (0.5) and previous-frame pixels are not counted.
#include "UiMask.hlsli"

cbuffer Params : register(b0)
{
    float Threshold;
    uint Radius;
    uint Width;
    uint Height;
    uint HasPrevious;
    uint Cleanup;
    uint UseHdr;
    uint Pad;
};

Texture2D<float4> PreviousPaste : register(t2);
Texture2D<float4> HdrFinalTexture : register(t3);
RWTexture2D<float4> PasteTexture : register(u0);
RWByteAddressBuffer Coverage : register(u1);

groupshared uint gFlagged;

[numthreads(UI_GROUP, UI_GROUP, 1)]
void CSMain(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID,
            uint gi : SV_GroupIndex)
{
    if (gi == 0)
        gFlagged = 0;

    const bool inside = id.x < Width && id.y < Height;
    // Barrier inside: all threads take part, the bounds test only guards the stores.
    const bool changed = UiWindowHasChange(gid.xy, gtid.xy, gi, uint2(Width, Height), Threshold, Radius);
    const bool isUi = changed && inside;

    if (isUi)
        InterlockedAdd(gFlagged, 1);

    GroupMemoryBarrierWithGroupSync();

    if (gi == 0 && gFlagged != 0)
        Coverage.InterlockedAdd(0, gFlagged);

    if (!inside)
        return;

    const int3 p = int3(id.xy, 0);
    const float3 colour = UseHdr != 0 ? HdrFinalTexture.Load(p).rgb : FinalTexture.Load(p).rgb;

    float4 result = float4(0.0, 0.0, 0.0, 0.0);
    if (isUi)
        result = float4(colour, 1.0);
    else if (Cleanup != 0 && HasPrevious != 0 && PreviousPaste.Load(p).a > 0.75)
        result = float4(colour, 0.5);

    PasteTexture[id.xy] = result;
}
