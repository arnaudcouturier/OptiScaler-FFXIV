// S10 UI mask (docs/HUDLESS.md section 7), shared by UiPasteProduce.hlsl and UiRecomposition.hlsl.
//
// Per pixel p of the real frame, F = final image (with UI), H = UI-free image of the same frame:
//   d(p)  = max(|F.r-H.r|, |F.g-H.g|, |F.b-H.b|)
//   windowMax(p) = the largest d in the square window of radius `radius` around p (clamped to the image)
//   isUi  = windowMax > threshold, that is: any pixel of the window has d > threshold
// A 16x16 group loads d for its (16 + 2r)^2 tile once into group shared memory, so a pixel costs about 2.25 texture
// reads instead of (2r+1)^2. Clamping the tile coordinates to the image gives the same answer as clamping the window.
// The window's maximum (not just its answer for one threshold) is returned so a caller can count the same window at
// several fixed difference levels without another texture read.

#define UI_GROUP 16
#define UI_MAX_RADIUS 4
#define UI_TILE_MAX (UI_GROUP + 2 * UI_MAX_RADIUS)

Texture2D<float4> FinalTexture : register(t0);
Texture2D<float4> HudlessTexture : register(t1);

groupshared float gUiDiff[UI_TILE_MAX * UI_TILE_MAX];

float UiPixelDiff(int2 p, uint2 size)
{
    p = clamp(p, int2(0, 0), int2(size) - 1);
    const float3 d = abs(FinalTexture.Load(int3(p, 0)).rgb - HudlessTexture.Load(int3(p, 0)).rgb);
    return max(max(d.r, d.g), d.b);
}

// Every thread of the group must call this (it contains a group barrier): do not return early before it. The barrier
// also publishes any group shared variable the caller wrote before the call.
float UiWindowMaxDiff(uint2 groupId, uint2 localId, uint groupIndex, uint2 size, uint radius)
{
    const int r = (int) min(radius, (uint) UI_MAX_RADIUS);
    const int dim = UI_GROUP + 2 * r;
    const int2 origin = int2(groupId) * UI_GROUP - r;

    const uint udim = (uint) dim;
    for (uint i = groupIndex; i < udim * udim; i += UI_GROUP * UI_GROUP)
        gUiDiff[i] = UiPixelDiff(origin + int2((int) (i % udim), (int) (i / udim)), size);

    GroupMemoryBarrierWithGroupSync();

    float windowMax = 0.0;
    for (int y = 0; y <= 2 * r; ++y)
    {
        for (int x = 0; x <= 2 * r; ++x)
            windowMax = max(windowMax, gUiDiff[(int) (localId.y + y) * dim + (int) localId.x + x]);
    }
    return windowMax;
}
