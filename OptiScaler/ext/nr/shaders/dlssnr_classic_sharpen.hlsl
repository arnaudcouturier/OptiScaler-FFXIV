// Classic-enlargement sharpening for the NR pass: RCAS over the resolved frame.
//
// The filter below is the plain RCAS (Robust Contrast Adaptive Sharpening) from upstream
// OptiScaler's own RCAS pass (shaders/rcas/precompile/rcas.hlsl, itself based on the
// RCAS-for-ReShade port of AMD FidelityFX RCAS), reduced to the static-sharpness path: no
// motion-adaptive sharpening, no debug tint, no contrast adaptation (Contrast = 0 upstream).
// Only the sharpness amount is a parameter, carried in the TransferStrength slot of the
// dispatch constants (see DlssNrConstants in pass/DlssNr_Common.h).
//
// Like the upstream filter, the normalized ring clamps at zero (negative scRGB channels clamp
// rather than sharpen), and an Inf pixel normalizes to NaN, which then spreads to its neighbours
// through the shared min/max ring.
//
// It is not upstream's separate RCAS pass: this runs inside the NR pass, through its own
// DispatchPass, reading only t0 and writing only u0 of the same descriptor table (six SRVs,
// two UAVs, one constant buffer) and root signature every other NR dispatch uses.
//
// Byte layout contract: field offsets match the prefix of DlssNrConstants (Mode, WhitePoint,
// Width, Height, TransferStrength), the way dlssnr_guide_match.hlsl does.
cbuffer Params : register(b0)
{
    uint gMode;      // DlssNrMode_ClassicSharpen, carried for the replay record
    float gWhite;    // unused here; keeps the layout against DlssNrConstants
    uint gWidth, gHeight;
    float gStrength; // the TransferStrength slot: the sharpen amount, 0 (a copy) to 1 (full)
};
Texture2D<float4> gSource : register(t0); // the resolved frame
RWTexture2D<float4> gTarget : register(u0); // the temp copy the result is staged into
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight) return;

    const float strength = saturate(gStrength);

    float4 e = gSource.Load(int3(id.xy, 0));

    // Strength zero is a copy: the pass still records the dispatch, but the picture is untouched.
    if (strength == 0.0)
    {
        gTarget[id.xy] = e;
        return;
    }

    // Clamp neighbor accesses at image borders (as upstream).
    int2 maxPixel = int2(gWidth - 1, gHeight - 1);
    int2 coordB = int2(id.x, max((int) id.y - 1, 0));
    int2 coordD = int2(max((int) id.x - 1, 0), id.y);
    int2 coordF = int2(min((int) id.x + 1, maxPixel.x), id.y);
    int2 coordH = int2(id.x, min((int) id.y + 1, maxPixel.y));

    float3 b = gSource.Load(int3(coordB, 0)).rgb;
    float3 d = gSource.Load(int3(coordD, 0)).rgb;
    float3 f = gSource.Load(int3(coordF, 0)).rgb;
    float3 h = gSource.Load(int3(coordH, 0)).rgb;

    // Only normalize HDR neighborhoods down; do not scale dark/LDR pixels up (as upstream).
    float localScale = max(
        max(e.r, max(e.g, e.b)),
        max(
            max(b.r, max(b.g, b.b)),
            max(
                max(d.r, max(d.g, d.b)),
                max(
                    max(f.r, max(f.g, f.b)),
                    max(h.r, max(h.g, h.b))
                )
            )
        )
    );

    localScale = max(localScale, 1.0);

    float3 en = max(e.rgb / localScale, 0.0);
    float3 bn = max(b / localScale, 0.0);
    float3 dn = max(d / localScale, 0.0);
    float3 fn = max(f / localScale, 0.0);
    float3 hn = max(h / localScale, 0.0);

    // Min and max of normalized ring (as upstream).
    float3 minRGB = min(min(bn, dn), min(fn, hn));
    float3 maxRGB = max(max(bn, dn), max(fn, hn));

    float2 peakC = float2(1.0, -4.0);

    // More numerically stable RCAS limiters (as upstream).
    float3 hitMin = minRGB / max(4.0 * maxRGB, 1e-5);
    float3 hitMax = (peakC.xxx - maxRGB) / max(4.0 * minRGB + peakC.yyy, -1e-5);

    float3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-0.1875, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * strength;

    float rcpL = rcp(4.0 * lobe + 1.0);
    float3 output = (((bn + dn + fn + hn) * lobe + en) * rcpL) * localScale;

    gTarget[id.xy] = float4(output, e.a);
}
