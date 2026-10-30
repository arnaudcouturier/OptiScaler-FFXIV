#pragma once
// CPU reference model of the Mode 1 (scene HDR) pixel path of OptiScaler/ext/hdr/shaders/Hdr10.hlsl, in double precision, in nits
// of linear Rec.709 before the BT.2020 conversion, saturation and PQ encoding (those stages are the same for both compositions).
//
//   LegacyCompose      the composition of 191c89b2 (two rules: world rule and HUD rule, sorted by the mask and a colour band)
//   ContinuousCompose  the continuous law ([HDR] LegacyComposition = false): one expression, continuous in every input
//
// The model is NOT the test oracle for the shader on its own: hdr_halo.cpp runs the compiled shader on WARP, checks that it agrees
// with this model (float versus double) over a dense sweep, and asserts the properties on the compiled shader.
//
// The law (per pixel; S = the final SDR pixel, B = the world beneath it, both in nits, decoded alike, contrast applied to both):
//   t   mask transmittance: 0 opaque tagged HUD, 1 "no TAGGED draw here" (which also means "no HUD" and cannot be told apart)
//   q   how much of the world is still visible = min(1, S/B, t)            (scalar, the smallest ratio over the channels)
//   A   the light the HUD adds = max(S - q*B, 0)                           (per channel)
//   a   HUD share of the pixel = max(1 - t, luma(A)/luma(S))               (the mask can only raise the estimate: continuous at t = 1)
//   C   HUD colour luma = min(luma(A)/a, PaperWhite)
//   U   HUD curve = 1 + (Peak/PaperWhite - 1) * UiExpansion * h(C/PaperWhite)
//   E   = MapScenePeak(q * B * G + A * U), scaled down to Peak if a channel exceeds it
// With no HUD (S == B) this is MapScenePeak(B * G), the old world rule. With t = 0 (S = A, q = 0) it is MapScenePeak(S * U), the old
// HUD rule. In between it never asks "world or HUD", so no step can appear.

#include <algorithm>
#include <array>
#include <cmath>

namespace halo
{
using V3 = std::array<double, 3>;

struct Config
{
    double paper = 203, peak = 1100, expansion = 0.5;
    double uiExpansion = -1; // < 0: follows `expansion` (the converter's "auto"); used by the continuous law only
    double saturation = 1, vibrance = 0; // the stage after the composition: the model skips it (a no-op at 1/0), the byte test sweeps it
    double contrast = 1;
    bool reshadeHighlights = false;
    bool referenceSrgb = false;
};

struct Pixel
{
    V3 final {}, reference {}, scene {}; // encoded: final and reference in 0..1, scene gamma 2.2 encoded and may exceed 1
    double mask = 1;                    // PreviewMask.r (t)
    bool hasMask = true;                // HasPreviewMask
};

inline double Sat(double x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
inline double Smooth(double a, double b, double x)
{
    const double t = Sat((x - a) / (b - a));
    return t * t * (3 - 2 * t);
}
inline double Srgb(double x) { return x <= .04045 ? x / 12.92 : std::pow((x + .055) / 1.055, 2.4); }
inline double Luma(const V3& v) { return .2126 * v[0] + .7152 * v[1] + .0722 * v[2]; }
inline double Max3(const V3& v) { return std::max(v[0], std::max(v[1], v[2])); }
inline V3 Mul(const V3& v, double k) { return { v[0] * k, v[1] * k, v[2] * k }; }

// The contrast sigmoid of the converter on a luma in 0..1.
inline double AdjustLuma(double y, double contrast)
{
    if (y > 0 && y < 1 && contrast != 1)
    {
        const double odds = (y / (1 - y)) / (.18 / .82);
        return 1 / (1 + (.82 / .18) * std::pow(odds, -contrast));
    }
    return y;
}
// ... applied to a linear colour in 0..1; returns the adjusted luma in `y`.
inline V3 Contrast(V3 rgb, double contrast, double& y)
{
    y = Luma(rgb);
    const double adjusted = AdjustLuma(y, contrast);
    if (adjusted != y)
    {
        rgb = Mul(rgb, adjusted / y);
        y = adjusted;
    }
    return rgb;
}
inline double H(double y)
{
    const double h = Sat((y - .18) / .82);
    return h * h * (3 - 2 * h);
}

inline V3 MapScenePeak(const Config& c, V3 v)
{
    const double high = Max3(v);
    if (high > c.paper)
    {
        const double x = high - c.paper, peak = c.peak - c.paper;
        v = Mul(v, (c.paper + peak * x / (x + peak)) / high);
    }
    return v;
}

// Everything both compositions need, computed as the shader does before it branches.
struct Inputs
{
    V3 S {};          // final pixel, nits, contrast applied ("rgb" of the shader)
    double y = 0;     // its luma after contrast, 0..1
    V3 S0 {}, B0 {};  // final and reference decoded alike (sRGB), nits, BEFORE contrast (continuous law: the law runs here)
    double kS = 1;    // contrast factor of the final pixel (adjusted luma / luma): the law's result is multiplied by it
    V3 neutral {};    // reference decoded for the scene gain
    V3 scale {};      // sceneLinear / neutral (scalar replicated when ReShadeHighlights)
    double coverage = 1; // the colour band (legacy only)
    double t = 1;
    V3 ui {};         // legacy HUD value
};

inline Inputs Prepare(const Config& c, const Pixel& p)
{
    Inputs in;
    V3 lin;
    for (int k = 0; k < 3; ++k)
        lin[k] = Srgb(Sat(p.final[k]));
    V3 rgb = Contrast(lin, c.contrast, in.y);
    in.S = Mul(rgb, c.paper);
    const double h = H(in.y);

    V3 refLin;
    for (int k = 0; k < 3; ++k)
        refLin[k] = Srgb(Sat(p.reference[k]));
    in.S0 = Mul(lin, c.paper);
    in.B0 = Mul(refLin, c.paper);
    const double y0 = Luma(lin);
    in.kS = (y0 > 0 && AdjustLuma(y0, c.contrast) != y0) ? AdjustLuma(y0, c.contrast) / y0 : 1.0;

    V3 sceneLinear, scale;
    for (int k = 0; k < 3; ++k)
    {
        sceneLinear[k] = std::pow(std::max(p.scene[k], 0.0), 2.2);
        in.neutral[k] = c.referenceSrgb ? Srgb(Sat(p.reference[k])) : std::pow(Sat(p.reference[k]), 2.2);
        scale[k] = sceneLinear[k] / std::max(in.neutral[k], 1e-4);
    }
    double difference = 0;
    for (int k = 0; k < 3; ++k)
        difference = std::max(difference, std::fabs(Sat(p.final[k]) - Sat(p.reference[k])));
    in.coverage = 1 - Smooth(.08, .25, difference);
    if (c.reshadeHighlights)
    {
        const double finalPeak = std::max(p.final[0], std::max(p.final[1], p.final[2]));
        const double referencePeak = std::max(p.reference[0], std::max(p.reference[1], p.reference[2]));
        const double highlight = Smooth(.65, .95, std::min(finalPeak, Sat(referencePeak)));
        in.coverage = std::max(in.coverage, highlight);
        const double scenePeak = Max3(sceneLinear), neutralPeak = Max3(in.neutral);
        const double s = std::max(scenePeak / std::max(neutralPeak, 1e-4), 1.0);
        scale = { s, s, s };
    }
    in.scale = scale;
    in.t = p.hasMask ? Sat(p.mask) : 1.0;
    in.ui = Mul(in.S, 1 + (c.peak / c.paper - 1) * c.expansion * h);
    return in;
}

inline V3 ClampPeak(const Config& c, V3 e)
{
    const double high = Max3(e);
    if (high > c.peak)
        e = Mul(e, c.peak / high);
    return e;
}

inline V3 LegacyCompose(const Config& c, const Pixel& p)
{
    const Inputs in = Prepare(c, p);
    const V3 world = { in.S[0] * std::max(in.scale[0], 1.0), in.S[1] * std::max(in.scale[1], 1.0), in.S[2] * std::max(in.scale[2], 1.0) };
    if (in.t > 0 && in.t < 1)
    {
        V3 bg, bgHdr, bgSdr, extended;
        for (int k = 0; k < 3; ++k)
            bg[k] = in.neutral[k] * c.paper;
        for (int k = 0; k < 3; ++k)
            bgHdr[k] = bg[k] * std::max(in.scale[k], 1.0);
        bgHdr = MapScenePeak(c, bgHdr);
        bgSdr = MapScenePeak(c, bg);
        const V3 mapped = MapScenePeak(c, in.ui);
        for (int k = 0; k < 3; ++k)
            extended[k] = mapped[k] + std::max(bgHdr[k] - bgSdr[k], 0.0) * in.coverage * in.t;
        return ClampPeak(c, extended);
    }
    V3 mixed;
    for (int k = 0; k < 3; ++k)
        mixed[k] = in.ui[k] + (world[k] - in.ui[k]) * (in.coverage * in.t);
    return MapScenePeak(c, mixed);
}

// Share of the visible world that extra light may add and still be read as a grade of the world (a ReShade tint or brightening),
// not as HUD light. Mirrors kGradeTolerance in Hdr10.hlsl.
inline constexpr double kGradeTolerance = 0.5;
// The tolerance fades out as the mask says "tagged HUD here": it is multiplied by t^kMaskPower (Hdr10.hlsl: pow(t, 8)), so it is
// full at t = 1 and small below t = 0.7; the fade is continuous, no step at t = 1.
inline constexpr double kMaskPower = 8;

struct Law
{
    double q = 1, a = 0, C = 0, U = 1;
    V3 A {}, Ag {}, Au {}; // extra light; the part read as grade (world gain); the part read as HUD (HUD curve)
};

inline Law Decompose(const Config& c, const Inputs& in)
{
    Law l;
    double ratio = 1;
    for (int k = 0; k < 3; ++k)
        ratio = std::min(ratio, in.S0[k] / std::max(in.B0[k], 1e-4));
    l.q = std::min(std::min(ratio, 1.0), in.t);
    for (int k = 0; k < 3; ++k)
        l.A[k] = std::max(in.S0[k] - l.q * in.B0[k], 0.0);
    for (int k = 0; k < 3; ++k)
    {
        l.Ag[k] = std::min(l.A[k], kGradeTolerance * l.q * std::pow(in.t, kMaskPower) * in.B0[k]);
        l.Au[k] = l.A[k] - l.Ag[k];
    }
    const double sl = Luma(in.S0), al = Luma(l.Au);
    l.a = std::max(1 - in.t, sl > 1e-4 ? al / sl : 0.0);
    l.C = l.a > 1e-6 ? std::min(al / l.a, c.paper) : 0.0;
    const double uiExp = c.uiExpansion < 0 ? c.expansion : c.uiExpansion;
    l.U = 1 + (c.peak / c.paper - 1) * uiExp * H(AdjustLuma(l.C / c.paper, c.contrast));
    return l;
}

inline V3 ContinuousCompose(const Config& c, const Pixel& p)
{
    const Inputs in = Prepare(c, p);
    const Law l = Decompose(c, in);
    V3 e;
    for (int k = 0; k < 3; ++k)
        e[k] = in.kS * ((l.q * in.B0[k] + l.Ag[k]) * std::max(in.scale[k], 1.0) + l.Au[k] * l.U);
    return ClampPeak(c, MapScenePeak(c, e));
}

// The last stage of the converter for both compositions: BT.2020 matrix, then a per channel clamp to the peak. Engines report these BT.2020
// nits (what the PQ code carries); for a gray pixel they equal the Rec.709 value (the matrix rows sum to 1).
inline V3 OutputStage(const Config& c, const V3& v)
{
    const V3 b = { .627404 * v[0] + .329282 * v[1] + .043314 * v[2], .069097 * v[0] + .919540 * v[1] + .011362 * v[2],
                   .016391 * v[0] + .088013 * v[1] + .895595 * v[2] };
    return { std::min(b[0], c.peak), std::min(b[1], c.peak), std::min(b[2], c.peak) };
}

inline V3 Compose(const Config& c, const Pixel& p, bool continuous) { return continuous ? ContinuousCompose(c, p) : LegacyCompose(c, p); }

// A gray pixel helper for the strips: encoded values for final / reference, scene value as an exposure scale (scene = reference * s^(1/2.2)).
inline Pixel Gray(double finalEnc, double referenceEnc, double sceneScale, double mask = 1, bool hasMask = true)
{
    Pixel p;
    const double sceneEnc = referenceEnc * std::pow(sceneScale, 1 / 2.2);
    for (int k = 0; k < 3; ++k)
    {
        p.final[k] = finalEnc;
        p.reference[k] = referenceEnc;
        p.scene[k] = sceneEnc;
    }
    p.mask = mask;
    p.hasMask = hasMask;
    return p;
}
} // namespace halo
