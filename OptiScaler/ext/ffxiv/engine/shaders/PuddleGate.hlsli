// The exposure gate, the one HLSL copy: pools only where the engine's own ground is wet. Included by GlossPuddles.hlsl (the gloss stage) and by
// PuddleReflection.hlsl (the tone stage), so the two stages cannot gate differently. GlossPuddlesRules.h holds the C++ copy (Exposure) and the
// reasons for every number; a host test pins that the constants here are that header's, and that neither shader declares a gate constant of its own.
//
//   exposure = smoothstep(low, high, gloss / (k * max(saturate(wetness), floor)))
//
// `engineGloss` is the alpha of the engine's own material pack (G-buffer 1) at the texel: about 0.83 x the wetness on ground that is rained on and 0
// under cover. A NaN or negative value is not exposed ground and reads as 0; the clause is the mirror of the rules' and cannot change a texel here,
// because the pack is an 8-bit BGRA texture (both stages refuse any other format before they read it) whose alpha is always a number in 0..1.
static const float kGateK = 0.83;
static const float kGateWetnessFloor = 0.05;
static const float kGateBandLow = 0.15;
static const float kGateBandHigh = 0.5;

float PuddleExposure(float engineGloss, float wetness)
{
    if (!(engineGloss >= 0.0))
        return 0.0;
    return smoothstep(kGateBandLow, kGateBandHigh, engineGloss / (kGateK * max(saturate(wetness), kGateWetnessFloor)));
}
