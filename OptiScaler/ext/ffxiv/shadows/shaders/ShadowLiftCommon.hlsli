// Shadow mapping (ext/ffxiv/shadows): the shared remap and tent rules of the two shaders that apply them - the
// module's own pass (shaders/ShadowLift.hlsl) and the contact shadows pass's combine stage
// (ext/ffxiv/relight/shaders/ContactShadows.hlsl). ONE source for both, so the WARP tests' CPU reference pins one
// rule to one shape and a shader that weights a wrong offset is refused by the same test as a wrong distance.
// Included by both files by relative path; nothing else includes it.
//
// The arithmetic is the report's (the design report 2.2, asm 649: the light draw squares the mask it samples):
// the remap is defined on the SQUARED mask (m'^2 = m^2 + (1-m^2)*Lift), the copy carries sqrt(m'^2) and the light
// draw's own square lands back on m'^2, and a lift at or below the identity epsilon is the byte-exact passthrough
// (the engine's own mask, whatever the softness key says - the tent feathers the lift, it does not feather the
// default).

#define SHADOW_LIFT_IDENTITY_EPSILON 0.001

// The tent weight of a neighbour at (dx, dy) mask pixels with the reach S: max(0, 1 - d/S) with d the pixel
// distance. A reach of at most one pixel touches the 3x3 kernel, two the 5x5 one; zero outside the reach; S <= 0 is
// the texel itself with weight 1.
float ShadowLiftTentWeight(float dx, float dy, float softness)
{
    if (!(softness > 0.0))
        return dx == 0.0 && dy == 0.0 ? 1.0 : 0.0;
    const float d = sqrt(dx * dx + dy * dy);
    if (!(d < softness))
        return 0.0;
    const float w = 1.0 - d / softness;
    return w > 0.0 ? w : 0.0;
}

// The remap on the squared mask, for one channel value: the square root of the saturated lift of the channel's own
// square (the value the copy must carry), guarded so an identity lift is the value itself with no arithmetic.
float ShadowLiftRemappedCopy(float channel, float lift)
{
    if (!(lift > SHADOW_LIFT_IDENTITY_EPSILON))
        return channel;
    const float squared = channel * channel;
    const float lifted = squared + (1.0 - squared) * lift;
    const float clamped = clamp(lifted, 0.0, 1.0);
    return clamped <= 0.0 ? 0.0 : clamped >= 1.0 ? 1.0 : sqrt(clamped);
}

// One texel of the lifted copy, from the mask's raw float4, its texture, the texel and the lift's values. The tent
// loads 5x5 with the reach, dropping the texels that fall outside the allocation (an out-of-range Load reads zero,
// which is not a shadowed texel but nothing at all, so the weights renormalise over the ones the kernel carried).
// With the softness at zero the texel is its own remap, no neighbours loaded.
float4 ShadowLiftTexel(float4 mask, Texture2D<float4> maskTexture, int2 at, int2 size, float lift, float softness)
{
    if (!(softness > 0.0))
        return float4(ShadowLiftRemappedCopy(mask.r, lift), ShadowLiftRemappedCopy(mask.g, lift),
                      ShadowLiftRemappedCopy(mask.b, lift), mask.a);
    float sumW = 0.0, sumR = 0.0, sumG = 0.0, sumB = 0.0;
    [unroll]
    for (int dy = -2; dy <= 2; ++dy)
    {
        [unroll]
        for (int dx = -2; dx <= 2; ++dx)
        {
            const float w = ShadowLiftTentWeight(float(dx), float(dy), softness);
            if (!(w > 0.0))
                continue;
            const int2 neighbourAt = at + int2(dx, dy);
            if (neighbourAt.x < 0 || neighbourAt.y < 0 || neighbourAt.x >= size.x || neighbourAt.y >= size.y)
                continue;
            const float4 neighbour = maskTexture.Load(int3(neighbourAt, 0));
            sumW += w;
            sumR += w * ShadowLiftRemappedCopy(neighbour.r, lift);
            sumG += w * ShadowLiftRemappedCopy(neighbour.g, lift);
            sumB += w * ShadowLiftRemappedCopy(neighbour.b, lift);
        }
    }
    if (!(sumW > 0.0))
        return float4(ShadowLiftRemappedCopy(mask.r, lift), ShadowLiftRemappedCopy(mask.g, lift),
                      ShadowLiftRemappedCopy(mask.b, lift), mask.a);
    return float4(sumR / sumW, sumG / sumW, sumB / sumW, mask.a);
}
