// S10 paste image producer (docs/HUDLESS.md section 7). One dispatch per real frame:
//   isUi                                  -> (colour, 1.0)
//   cleanup && previous alpha > 0.75      -> (colour, 0.5)   clears ghosts of UI that just closed
//   otherwise                             -> (0, 0, 0, 0)
// colour = FinalTexture, or HdrFinalTexture (PQ) when UseHdr != 0 (the mask is always computed on the SDR pair).
// The number of isUi pixels is added to Coverage[0] (one global atomic per group); the CPU zeroes it before the dispatch.
// Cleanup (0.5) and previous-frame pixels are not counted.
//
// Coverage (u1) is 4 x uint32, all zeroed by the CPU before the dispatch:
//   [0] pixels the mask flags at the configured Threshold (what the coverage guard reads)
//   [1] pixels whose windowed difference reaches 0.008 but not 0.05
//   [2] ... 0.05 but not 0.2
//   [3] ... 0.2
// A pixel counts in the highest band it reaches, so the three amplitude shares the CPU reports are [1]+[2]+[3], [2]+[3]
// and [3]. The bands are fixed and do not follow Threshold, so the numbers are comparable between sessions and settings.
#include "UiMask.hlsli"

// Amplitude bands of the mask, per channel, 0..1 on the SDR pair (docs/HUDLESS.md section 7).
#define UI_AMPLITUDE_LOW 0.008
#define UI_AMPLITUDE_MID 0.05
#define UI_AMPLITUDE_HIGH 0.2

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
groupshared uint gBandLow;
groupshared uint gBandMid;
groupshared uint gBandHigh;

[numthreads(UI_GROUP, UI_GROUP, 1)]
void CSMain(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID,
            uint gi : SV_GroupIndex)
{
    if (gi == 0)
    {
        gFlagged = 0;
        gBandLow = 0;
        gBandMid = 0;
        gBandHigh = 0;
    }

    const bool inside = id.x < Width && id.y < Height;
    // Barrier inside: all threads take part, the bounds test only guards the stores.
    const float windowMax = UiWindowMaxDiff(gid.xy, gtid.xy, gi, uint2(Width, Height), Radius);
    const bool isUi = windowMax > Threshold && inside;

    if (isUi)
        InterlockedAdd(gFlagged, 1);

    // Amplitude band of this pixel, one shared atomic in total: the highest fixed level its windowed difference reaches.
    if (inside)
    {
        if (windowMax > UI_AMPLITUDE_HIGH)
            InterlockedAdd(gBandHigh, 1);
        else if (windowMax > UI_AMPLITUDE_MID)
            InterlockedAdd(gBandMid, 1);
        else if (windowMax > UI_AMPLITUDE_LOW)
            InterlockedAdd(gBandLow, 1);
    }

    GroupMemoryBarrierWithGroupSync();

    if (gi == 0)
    {
        if (gFlagged != 0)
            Coverage.InterlockedAdd(0, gFlagged);
        if (gBandLow != 0)
            Coverage.InterlockedAdd(4, gBandLow);
        if (gBandMid != 0)
            Coverage.InterlockedAdd(8, gBandMid);
        if (gBandHigh != 0)
            Coverage.InterlockedAdd(12, gBandHigh);
    }

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
