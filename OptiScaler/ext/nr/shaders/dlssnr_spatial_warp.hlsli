// Adapted from PeripheralWarp shaders/peripheral_warp_common.hlsli at
// 64902dd6a02460e5f6b778504ec2a4005faf4d9c (MIT; see external/peripheral_warp/LICENSE).

// The including shader defines PW_FOVEATED: true when the foveated profile is selected. The per-side slots then carry the
// foveated parameters instead of the peripheral ones (centre -> h, work -> F, compression -> e, edgeSlope -> G(h + F)), the
// lengths are pixels and not fractions of the half-span. Keep Fv* in step with ext/nr/spatial/FoveatedWarp.h.
#ifndef PW_FOVEATED
#error PW_FOVEATED must be defined before including dlssnr_spatial_warp.hlsli
#endif

float FvPackedDistance(float r, float h, float F, float e, float gEnd)
{
    if (r <= h) return r;
    float d = r - h;
    if (d < F)
    {
        float t = d / F;
        float t3 = t * t * t;
        return h + d - (1.0 - e) * F * (t3 - 0.5 * t3 * t);
    }
    return gEnd + e * (d - F);
}

float FvUnpackedDistance(float g, float h, float F, float e, float gEnd)
{
    if (g <= h) return g;
    if (F <= 0.0 || g >= gEnd) return h + F + (g - gEnd) / e;
    float q = 0.5 * (1.0 + e);
    float target = (g - h) / F;
    float t = target / q;
    [unroll] for (int i = 0; i < 4; ++i)
    {
        float t2 = t * t;
        float packedLength = t - (1.0 - e) * (t2 * t - 0.5 * t2 * t2);
        float slope = 1.0 - (1.0 - e) * t2 * (3.0 - 2.0 * t);
        t = clamp(t - (packedLength - target) / slope, 0.0, 1.0);
    }
    return h + F * t;
}

float PwPackRadius(float radius, float center, float work, float compression, float edgeSlope)
{
    radius = abs(radius);
    if (work >= 1.0 || center >= 1.0 || radius <= center) return radius;
    if (radius > 1.0) return work + (radius - 1.0) * edgeSlope;
    float t = (radius - center) / (1.0 - center);
    return center + (work - center) * t / (compression + (1.0 - compression) * t);
}

float PwUnpackRadius(float radius, float center, float work, float compression, float edgeSlope)
{
    radius = abs(radius);
    if (work >= 1.0 || center >= 1.0 || radius <= center) return radius;
    if (radius > work) return 1.0 + (radius - work) / edgeSlope;
    float y = (radius - center) / (work - center);
    float t = compression * y / (1.0 - (1.0 - compression) * y);
    return center + (1.0 - center) * t;
}

struct PwAxis
{
    float nativeExtent, workExtent, bandCenter, workCenter;
    float2 halfSpan, center, work, compression, edgeSlope;
    float scale;
};

PwAxis PwAxisX()
{
    PwAxis a;
    a.nativeExtent = gNativeWorkSize.x; a.workExtent = gNativeWorkSize.z;
    a.bandCenter = gBandCenter.x; a.workCenter = gBandCenter.z;
    a.halfSpan = float2(gSideNeg.x, gSidePos.x);
    a.center = float2(gSideNeg.z, gSidePos.z);
    a.work = float2(gSideWorkNeg.x, gSideWorkPos.x);
    a.compression = float2(gSideWorkNeg.z, gSideWorkPos.z);
    a.edgeSlope = float2(gSideEdgeSlope.x, gSideEdgeSlope.z);
    a.scale = gWorkScale.x;
    return a;
}

PwAxis PwAxisY()
{
    PwAxis a;
    a.nativeExtent = gNativeWorkSize.y; a.workExtent = gNativeWorkSize.w;
    a.bandCenter = gBandCenter.y; a.workCenter = gBandCenter.w;
    a.halfSpan = float2(gSideNeg.y, gSidePos.y);
    a.center = float2(gSideNeg.w, gSidePos.w);
    a.work = float2(gSideWorkNeg.y, gSideWorkPos.y);
    a.compression = float2(gSideWorkNeg.w, gSideWorkPos.w);
    a.edgeSlope = float2(gSideEdgeSlope.y, gSideEdgeSlope.w);
    a.scale = gWorkScale.y;
    return a;
}

float PwPackAxis(float pixel, PwAxis a)
{
    float delta = pixel - a.bandCenter;
    bool positive = delta >= 0.0;
    float span = positive ? a.halfSpan.y : a.halfSpan.x;
    float center = positive ? a.center.y : a.center.x;
    float work = positive ? a.work.y : a.work.x;
    float compression = positive ? a.compression.y : a.compression.x;
    float edgeSlope = positive ? a.edgeSlope.y : a.edgeSlope.x;
    if (PW_FOVEATED)
        return a.workCenter + (positive ? 1.0 : -1.0) *
            FvPackedDistance(abs(delta), center, work, compression, edgeSlope) * a.scale;
    float radius = abs(delta) / span;
    return a.workCenter + (positive ? 1.0 : -1.0) *
        PwPackRadius(radius, center, work, compression, edgeSlope) * span * a.scale;
}

float PwUnpackAxis(float pixel, PwAxis a)
{
    float delta = pixel - a.workCenter;
    bool positive = delta >= 0.0;
    float span = positive ? a.halfSpan.y : a.halfSpan.x;
    float center = positive ? a.center.y : a.center.x;
    float work = positive ? a.work.y : a.work.x;
    float compression = positive ? a.compression.y : a.compression.x;
    float edgeSlope = positive ? a.edgeSlope.y : a.edgeSlope.x;
    if (PW_FOVEATED)
        return a.bandCenter + (positive ? 1.0 : -1.0) *
            FvUnpackedDistance(abs(delta) / a.scale, center, work, compression, edgeSlope);
    float radius = abs(delta) / (span * a.scale);
    return a.bandCenter + (positive ? 1.0 : -1.0) *
        PwUnpackRadius(radius, center, work, compression, edgeSlope) * span;
}

float2 PwPack(float2 nativePixel)
{
    return float2(PwPackAxis(nativePixel.x, PwAxisX()),
                  PwPackAxis(nativePixel.y, PwAxisY()));
}

float2 PwUnpack(float2 workPixel)
{
    return float2(PwUnpackAxis(workPixel.x, PwAxisX()),
                  PwUnpackAxis(workPixel.y, PwAxisY()));
}

float2 PwFootprint(float2 workPixel)
{
    return max(abs(PwUnpack(workPixel + 0.5) - PwUnpack(workPixel - 0.5)), 1.0.xx);
}
