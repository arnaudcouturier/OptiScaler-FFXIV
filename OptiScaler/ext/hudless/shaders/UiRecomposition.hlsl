// S10 recomposition mask (docs/HUDLESS.md section 7): the "UI colour + alpha" image DLSS-G / XeFG take as UI input.
// (final colour, 1) where the UI mask is set, (0, 0, 0, 0) elsewhere. Alpha is strictly 0 or 1, so the result is the
// same for premultiplied and straight-alpha consumers. Output R8G8B8A8_UNORM.
#include "UiMask.hlsli"

cbuffer Params : register(b0)
{
    float Threshold;
    uint Radius;
    uint Width;
    uint Height;
};

RWTexture2D<float4> UiTexture : register(u0);

[numthreads(UI_GROUP, UI_GROUP, 1)]
void CSMain(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID,
            uint gi : SV_GroupIndex)
{
    const bool changed = UiWindowHasChange(gid.xy, gtid.xy, gi, uint2(Width, Height), Threshold, Radius);

    if (id.x >= Width || id.y >= Height)
        return;

    UiTexture[id.xy] = changed ? float4(FinalTexture.Load(int3(id.xy, 0)).rgb, 1.0) : float4(0.0, 0.0, 0.0, 0.0);
}
