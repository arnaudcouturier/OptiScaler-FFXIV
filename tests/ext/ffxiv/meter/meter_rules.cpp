// Effect meter (METER): the pure rules on the host (no device).
//
// What a wrong rule would break, and what is checked here:
//   the measure: the change is the largest channel's |level(out) - level(in)| in display terms (the stored value over the frame's white, clamped at white) for the
//   chain and in the texture's own levels for the pre-light stages; EXACTLY 8 levels is not visible, a hair more is; the stage's channel mask decides (the
//   occlusion is the red channel only, the gloss is alpha only); a NaN texel counts as nothing; the white scales the levels (a different white moves the share);
//   the regions: occluded / lit / shaded / reflecting / window are decided at their stated thresholds, on the engine's OR this module's texel;
//   the geometry: the block is the smallest power of two that fits the target (1080p -> 8, 4K -> 16, a small box -> 1 = exact), every tap of a block is in its
//   own stratum, the taps are deterministic and move with the seed, an exact geometry visits every texel once;
//   the sampled share equals the full diff: tap totals on structured images land within 1 % (absolute) of the CPU full diff, and an exact geometry is equal;
//   the schedule: each effect once in ~3 s, staggered (never two samples in one frame, none while one is in flight, a lost result is dropped after 2 s);
//   the frame book: counts are per RENDERED frame (many draws of a frame are one), ran beats skipped, reasons are keyed with their numbers folded, stale tokens
//   are ignored, key-off effects are never "reached", the window closes into the total;
//   the clock: the effects' lines every 10 s, the summary at 60 s and then every 5 minutes;
//   the wording: the summary line is the brief's sentence.
#include "check.h"

#include "ext/ffxiv/meter/MeterRules.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <vector>

namespace mt = ext::ffxiv::meter;
using mt::StageId;

namespace
{
bool Near(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }
float QuietNan() { return std::numeric_limits<float>::quiet_NaN(); }

// A texel with all four channels the same.
void Set4(float* t, float v)
{
    t[0] = t[1] = t[2] = t[3] = v;
}

// A synthetic picture: 256 x 128, the left half carries a change of `step` stored units (an effect), the right half none; plus a small bright square.
struct Picture
{
    uint32_t w = 256, h = 128;
    float step = 0.0f;
    void At(uint32_t x, uint32_t y, float* in, float* out) const
    {
        const float base = 0.2f + 0.3f * float(x) / float(w);
        in[0] = base;
        in[1] = base * 0.9f;
        in[2] = base * 0.8f;
        in[3] = 1.0f;
        const bool changed = x < w / 2 && y >= 16;
        out[0] = in[0] + (changed ? step : 0.0f);
        out[1] = in[1];
        out[2] = in[2];
        out[3] = 1.0f;
    }
};
} // namespace

int main()
{
    // ---- the table ---------------------------------------------------------------------------------------------------------------------------------
    {
        CHECK_EQ(mt::kEffectCount, uint32_t(ext::ffxiv::diag::kStageCount));
        for (uint32_t i = 0; i < mt::kEffectCount; ++i)
        {
            const mt::EffectInfo& info = mt::kEffects[i];
            CHECK_EQ(uint32_t(info.id), i);
            CHECK(info.name != nullptr && info.name[0] != '\0');
            CHECK(info.regionText != nullptr && info.regionText[0] != '\0');
            CHECK(info.channels != 0);
            // the metered flag follows the registry: the flare makes no image
            CHECK_EQ(info.metered, ext::ffxiv::diag::StageInfoOf(info.id).images);
            // the chain stages are measured in display terms, the pre-light stages in their own levels
            CHECK_EQ(info.unit == mt::Unit::Display, ext::ffxiv::diag::StageInfoOf(info.id).group == ext::ffxiv::diag::StageGroup::Chain);
        }
        // the channels the stages write: the occlusion is red, the gloss is alpha, the sun masks are R8
        CHECK_EQ(mt::InfoOf(StageId::Ao).channels, mt::kChannelR);
        CHECK_EQ(mt::InfoOf(StageId::Gloss).channels, mt::kChannelA);
        CHECK_EQ(mt::InfoOf(StageId::ContactShadows).channels, mt::kChannelR);
        CHECK_EQ(mt::InfoOf(StageId::ShadowLift).channels, mt::kChannelR);
        CHECK_EQ(mt::InfoOf(StageId::Ssr).channels, mt::kChannelsRgb);
        // the stated regions
        CHECK(mt::InfoOf(StageId::Ao).region == mt::RegionKind::Occluded);
        CHECK(mt::InfoOf(StageId::ContactShadows).region == mt::RegionKind::Lit);
        CHECK(mt::InfoOf(StageId::ShadowLift).region == mt::RegionKind::Shaded);
        CHECK(mt::InfoOf(StageId::Ssr).region == mt::RegionKind::Reflecting);
        CHECK(mt::InfoOf(StageId::Atmosphere).region == mt::RegionKind::Window);
        CHECK(mt::kVisibleLevels == 8.0f);
    }

    // ---- the change ---------------------------------------------------------------------------------------------------------------------------------
    {
        float in[4], out[4];
        // display terms: stored 0.419 at white 0.838 is half of display white
        CHECK(Near(mt::LevelOf(0.419f, mt::Unit::Display, 0.838f), 127.5, 0.01));
        // clamped at white: a pixel above white is white
        CHECK(Near(mt::LevelOf(2.0f, mt::Unit::Display, 0.838f), 255.0, 1e-6));
        CHECK(Near(mt::LevelOf(-1.0f, mt::Unit::Display, 0.838f), 0.0, 1e-6));
        // unorm: its own levels, the white is not used
        CHECK(Near(mt::LevelOf(0.5f, mt::Unit::Unorm, 0.123f), 127.5, 1e-4));

        // the threshold is strict: 7.8 levels is not visible, 8.2 is (display white 1.0, so one level is 1/255 of a stored unit)
        Set4(in, 0.5f);
        Set4(out, 0.5f + 7.8f / 255.0f);
        mt::Totals t;
        mt::Params p = mt::ParamsOf(StageId::Gi, 1.0f);
        mt::Accumulate(t, in, out, p);
        CHECK(t.region == 1.0 && t.visible == 0.0);
        Set4(out, 0.5f + 8.2f / 255.0f);
        t = mt::Totals {};
        mt::Accumulate(t, in, out, p);
        CHECK(t.region == 1.0 && t.visible == 1.0);
        // exactly the threshold is NOT visible, the next float up is (the comparison is strict, whatever the float noise of a level computation)
        CHECK(!mt::IsVisible(8.0f));
        CHECK(mt::IsVisible(std::nextafter(8.0f, 100.0f)));
        CHECK(!mt::IsVisible(std::nextafter(8.0f, 0.0f)));
        CHECK(!mt::IsVisible(0.0f));
        // a change of the same size in the other direction counts the same
        Set4(out, 0.5f - 8.2f / 255.0f);
        t = mt::Totals {};
        mt::Accumulate(t, in, out, p);
        CHECK(t.visible == 1.0);

        // the largest channel decides: a change in blue alone is the change
        Set4(in, 0.3f);
        Set4(out, 0.3f);
        out[2] += 0.2f;
        CHECK(Near(mt::ChangeLevels(in, out, mt::kChannelsRgb, mt::Unit::Display, 1.0f), 0.2 * 255.0, 0.01));
        // the channel mask: the occlusion is red only, a change in green is no change; the gloss is alpha only
        Set4(in, 0.5f);
        Set4(out, 0.5f);
        out[1] = 0.9f;
        CHECK(Near(mt::ChangeLevels(in, out, mt::kChannelR, mt::Unit::Unorm, 1.0f), 0.0, 1e-6));
        out[0] = 0.4f;
        CHECK(Near(mt::ChangeLevels(in, out, mt::kChannelR, mt::Unit::Unorm, 1.0f), 25.5, 0.01));
        Set4(out, 0.5f);
        out[3] = 0.6f;
        CHECK(Near(mt::ChangeLevels(in, out, mt::kChannelA, mt::Unit::Unorm, 1.0f), 25.5, 0.01));
        CHECK(Near(mt::ChangeLevels(in, out, mt::kChannelsRgb, mt::Unit::Unorm, 1.0f), 0.0, 1e-6));

        // a NaN texel is no change and no region: -1, and Accumulate counts the tap only
        Set4(in, 0.5f);
        Set4(out, 0.5f);
        out[1] = QuietNan();
        CHECK(mt::ChangeLevels(in, out, mt::kChannelsRgb, mt::Unit::Display, 1.0f) < 0.0f);
        t = mt::Totals {};
        mt::Accumulate(t, in, out, p);
        CHECK(t.taps == 1.0 && t.region == 0.0 && t.visible == 0.0 && t.sum == 0.0);

        // the white scales the levels: the same stored change is a bigger level change at a smaller white
        Set4(in, 0.30f);
        Set4(out, 0.34f); // 0.04 stored
        const float atHigh = mt::ChangeLevels(in, out, mt::kChannelsRgb, mt::Unit::Display, 0.838f);
        const float atLow = mt::ChangeLevels(in, out, mt::kChannelsRgb, mt::Unit::Display, 0.700f);
        CHECK(atLow > atHigh && Near(atHigh, 0.04 / 0.838 * 255.0, 0.01) && Near(atLow, 0.04 / 0.700 * 255.0, 0.01));
        // an unusable white falls back to the captured measurement
        CHECK(mt::ParamsOf(StageId::Gi, 0.0f).white == mt::kFallbackWhite);
        CHECK(mt::ParamsOf(StageId::Gi, QuietNan()).white == mt::kFallbackWhite);
        CHECK(mt::ParamsOf(StageId::Gi, 0.7f).white == 0.7f);
        // a pre-light stage ignores the white
        CHECK(mt::ParamsOf(StageId::Ao, 0.7f).white == 1.0f);
    }

    // ---- the regions --------------------------------------------------------------------------------------------------------------------------------
    {
        float in[4], out[4];
        const auto L = [](int level) { return float(level) / 255.0f; };
        // window: always
        Set4(in, 0.5f);
        Set4(out, 0.5f);
        CHECK(mt::InRegion(mt::RegionKind::Window, in, out, mt::kChannelsRgb, mt::Unit::Display, 1.0f));
        // occluded (red): white in both is outside; one of the two below white is inside
        Set4(in, L(255));
        Set4(out, L(255));
        CHECK(!mt::InRegion(mt::RegionKind::Occluded, in, out, mt::kChannelR, mt::Unit::Unorm, 1.0f));
        out[0] = L(254);
        CHECK(mt::InRegion(mt::RegionKind::Occluded, in, out, mt::kChannelR, mt::Unit::Unorm, 1.0f));
        out[0] = L(255);
        in[0] = L(200); // the engine's own occlusion there, this module's texture white
        CHECK(mt::InRegion(mt::RegionKind::Occluded, in, out, mt::kChannelR, mt::Unit::Unorm, 1.0f));
        // lit: nothing in either is outside; 7 levels is outside, 8 is inside, either side counts
        Set4(in, 0.0f);
        Set4(out, 0.0f);
        CHECK(!mt::InRegion(mt::RegionKind::Lit, in, out, mt::kChannelR, mt::Unit::Unorm, 1.0f));
        in[0] = L(7);
        CHECK(!mt::InRegion(mt::RegionKind::Lit, in, out, mt::kChannelR, mt::Unit::Unorm, 1.0f));
        in[0] = L(8);
        CHECK(mt::InRegion(mt::RegionKind::Lit, in, out, mt::kChannelR, mt::Unit::Unorm, 1.0f));
        in[0] = 0.0f;
        out[0] = L(200);
        CHECK(mt::InRegion(mt::RegionKind::Lit, in, out, mt::kChannelR, mt::Unit::Unorm, 1.0f));
        // shaded: fully lit in both is outside, 247 inside, 248 outside
        Set4(in, L(255));
        Set4(out, L(255));
        CHECK(!mt::InRegion(mt::RegionKind::Shaded, in, out, mt::kChannelR, mt::Unit::Unorm, 1.0f));
        out[0] = L(248);
        CHECK(!mt::InRegion(mt::RegionKind::Shaded, in, out, mt::kChannelR, mt::Unit::Unorm, 1.0f));
        out[0] = L(247);
        CHECK(mt::InRegion(mt::RegionKind::Shaded, in, out, mt::kChannelR, mt::Unit::Unorm, 1.0f));
        // reflecting: black in both is outside; 1 level in either is inside
        Set4(in, 0.0f);
        Set4(out, 0.0f);
        CHECK(!mt::InRegion(mt::RegionKind::Reflecting, in, out, mt::kChannelsRgb, mt::Unit::Unorm, 1.0f));
        out[2] = L(1);
        CHECK(mt::InRegion(mt::RegionKind::Reflecting, in, out, mt::kChannelsRgb, mt::Unit::Unorm, 1.0f));
        // the region decides on the stage's channels only: a bright green does not make the occlusion "lit"
        Set4(in, 0.0f);
        Set4(out, 0.0f);
        out[1] = 1.0f;
        CHECK(!mt::InRegion(mt::RegionKind::Lit, in, out, mt::kChannelR, mt::Unit::Unorm, 1.0f));
        // a texel outside the region adds a tap and nothing else
        mt::Totals t;
        Set4(in, 0.0f);
        Set4(out, 0.0f);
        out[1] = 0.5f;
        mt::Accumulate(t, in, out, mt::ParamsOf(StageId::ContactShadows, 1.0f));
        CHECK(t.taps == 1.0 && t.region == 0.0 && t.sum == 0.0);
    }

    // ---- the geometry -------------------------------------------------------------------------------------------------------------------------------
    {
        const mt::Geometry hd = mt::GeometryFor(1920, 1080);
        CHECK_EQ(hd.block, 16u);
        CHECK_EQ(hd.binsX, 120u);
        CHECK_EQ(hd.binsY, 68u);
        CHECK_EQ(hd.Taps(), 16u);
        CHECK(!hd.Exact());
        CHECK(double(hd.binsX) * hd.binsY * hd.Taps() <= 131072.0); // the tap budget
        const mt::Geometry uhd = mt::GeometryFor(3840, 2160);
        CHECK_EQ(uhd.block, 32u);
        CHECK_EQ(uhd.binsX, 120u);
        CHECK_EQ(uhd.binsY, 68u);
        CHECK_EQ(uhd.cell, 8u);
        CHECK(double(uhd.binsX) * uhd.binsY * uhd.Taps() <= 131072.0); // the same tap budget at 4K
        const mt::Geometry small = mt::GeometryFor(64, 64);
        CHECK_EQ(small.block, 1u);
        CHECK(small.Exact());
        CHECK_EQ(small.Taps(), 1u);
        const mt::Geometry mid = mt::GeometryFor(300, 300);
        CHECK_EQ(mid.block, 4u);
        CHECK_EQ(mid.Taps(), 16u);
        CHECK(mid.Exact());
        const mt::Geometry ultra = mt::GeometryFor(5120, 1440);
        CHECK(ultra.Valid() && ultra.binsX <= mt::kMaxBinsSide && ultra.binsY <= mt::kMaxBinsSide && uint64_t(ultra.binsX) * ultra.binsY <= mt::kMaxBins);
        CHECK(!mt::GeometryFor(0, 100).Valid());
        CHECK(!mt::GeometryFor(100, 0).Valid());
        CHECK(!mt::GeometryFor(128 * 256 + 1, 10).Valid()); // wider than the biggest block reaches
        // every box in a range fits the target
        for (uint32_t w = 1; w <= 8192; w += 97)
            for (uint32_t h = 1; h <= 4608; h += 331)
            {
                const mt::Geometry g = mt::GeometryFor(w, h);
                CHECK(g.Valid());
                CHECK(g.binsX * g.block >= w && g.binsY * g.block >= h);
                CHECK(g.binsX <= mt::kMaxBinsSide && g.binsY <= mt::kMaxBinsSide && uint64_t(g.binsX) * g.binsY <= mt::kMaxBins);
                // and the next smaller block would not have fit (the smallest block is chosen)
                if (g.block > 1)
                {
                    const uint32_t b = g.block / 2;
                    const uint32_t bx = (w + b - 1) / b, by = (h + b - 1) / b;
                    CHECK(!(bx <= mt::kMaxBinsSide && by <= mt::kMaxBinsSide && uint64_t(bx) * by <= mt::kMaxBins));
                }
            }

        // an exact geometry visits every texel of a block once
        for (uint32_t block : { 1u, 2u, 4u })
        {
            mt::Geometry g;
            g.block = block;
            g.binsX = g.binsY = 1;
            g.tapsPerSide = block;
            g.cell = 1;
            std::set<uint32_t> seen;
            for (uint32_t k = 0; k < g.Taps(); ++k)
            {
                const mt::TapOffset o = mt::TapOf(g, 3, 5, k, 17);
                CHECK(o.x < block && o.y < block);
                seen.insert(o.y * block + o.x);
            }
            CHECK_EQ(uint32_t(seen.size()), block * block);
        }
        // a sparse geometry: every tap sits in its own stratum, and the jitter moves with the seed
        {
            const mt::Geometry g = mt::GeometryFor(1920, 1080);
            std::set<uint32_t> strata;
            bool moved = false;
            for (uint32_t k = 0; k < g.Taps(); ++k)
            {
                const mt::TapOffset a = mt::TapOf(g, 11, 7, k, 1);
                const mt::TapOffset b = mt::TapOf(g, 11, 7, k, 2);
                CHECK(a.x < g.block && a.y < g.block);
                CHECK_EQ(a.x / g.cell, k % g.tapsPerSide);
                CHECK_EQ(a.y / g.cell, k / g.tapsPerSide);
                strata.insert((a.y / g.cell) * g.tapsPerSide + a.x / g.cell);
                moved = moved || a.x != b.x || a.y != b.y;
                const mt::TapOffset again = mt::TapOf(g, 11, 7, k, 1);
                CHECK(again.x == a.x && again.y == a.y);
            }
            CHECK_EQ(uint32_t(strata.size()), g.Taps());
            CHECK(moved);
        }
    }

    // ---- the sampled share equals the full diff ---------------------------------------------------------------------------------------------------------
    {
        Picture pic;
        const auto fetch = [&pic](uint32_t x, uint32_t y, float* in, float* out) { pic.At(x, y, in, out); };
        // a change of 12 levels on the left half below row 16: the region is the window, the visible share is exactly (w/2 * (h-16)) / (w*h)
        pic.step = 12.0f / 255.0f * 0.838f;
        const mt::Params p = mt::ParamsOf(StageId::Gi, 0.838f);
        const mt::Totals full = mt::ReferenceTotals(pic.w, pic.h, p, fetch);
        CHECK(full.taps == double(pic.w) * pic.h && full.region == full.taps);
        CHECK(Near(full.Share(), double(pic.w / 2) * (pic.h - 16) / (double(pic.w) * pic.h), 1e-9));
        CHECK(Near(full.Mean(), 12.0 * full.Share(), 0.05));
        // the exact geometry (a 256 x 128 box fits at block 2: every texel is a tap) is the full diff
        const mt::Geometry g = mt::GeometryFor(pic.w, pic.h);
        CHECK(g.Exact());
        const mt::Totals exact = mt::TapTotals(pic.w, pic.h, g, 5, p, fetch);
        CHECK(exact.taps == full.taps && exact.region == full.region && exact.visible == full.visible && Near(exact.sum, full.sum, 1e-6 * full.sum));
        // a sparse geometry on a big picture: within 1 % (absolute) of the full diff
        Picture big;
        big.w = 1920;
        big.h = 1080;
        big.step = 20.0f / 255.0f * 0.838f;
        const auto fetchBig = [&big](uint32_t x, uint32_t y, float* in, float* out) { big.At(x, y, in, out); };
        const mt::Totals fullBig = mt::ReferenceTotals(big.w, big.h, p, fetchBig);
        const mt::Geometry gb = mt::GeometryFor(big.w, big.h);
        for (uint32_t seed : { 0u, 1u, 7u, 1234u })
        {
            const mt::Totals tap = mt::TapTotals(big.w, big.h, gb, seed, p, fetchBig);
            CHECK(Near(tap.Share(), fullBig.Share(), 0.01));
            CHECK(Near(tap.Mean(), fullBig.Mean(), 0.15));
            CHECK(Near(tap.Coverage(), 1.0, 1e-9));
        }
        // a region that is a small part of the box (the occlusion's occluded texels): coverage is the region over the box and the share is the region's
        Picture occ;
        occ.w = 512;
        occ.h = 256;
        const auto fetchOcc = [&occ](uint32_t x, uint32_t y, float* in, float* out) {
            const bool inside = x >= 100 && x < 200 && y >= 50 && y < 150; // 100 x 100 occluded texels
            Set4(in, inside ? 0.5f : 1.0f);
            Set4(out, inside ? (x < 150 ? 0.4f : 0.5f) : 1.0f); // half of them changed by 25 levels
        };
        const mt::Params po = mt::ParamsOf(StageId::Ao, 1.0f);
        const mt::Totals fullOcc = mt::ReferenceTotals(occ.w, occ.h, po, fetchOcc);
        CHECK(fullOcc.region == 10000.0 && Near(fullOcc.Share(), 0.5, 1e-9) && Near(fullOcc.Coverage(), 10000.0 / (512.0 * 256.0), 1e-9));
        const mt::Geometry go = mt::GeometryFor(occ.w, occ.h);
        const mt::Totals tapOcc = mt::TapTotals(occ.w, occ.h, go, 3, po, fetchOcc);
        CHECK(Near(tapOcc.Share(), 0.5, 0.01) && Near(tapOcc.Coverage(), fullOcc.Coverage(), 0.01));
        // key off: output equals input, share 0 and mean 0 exactly (no rounding noise), on a sparse geometry too
        pic.step = 0.0f;
        const mt::Totals off = mt::TapTotals(big.w, big.h, gb, 9, p, [&pic](uint32_t x, uint32_t y, float* in, float* out) { pic.At(x, y, in, out); });
        CHECK(off.region > 0.0 && off.visible == 0.0 && off.sum == 0.0);
    }

    // ---- Reduce -------------------------------------------------------------------------------------------------------------------------------------
    {
        // 3 x 2 texels in a row pitch of 16 floats (one texel of padding): the padding is not read
        float texels[32] {};
        for (uint32_t y = 0; y < 2; ++y)
            for (uint32_t x = 0; x < 3; ++x)
            {
                float* t = texels + y * 16 + x * 4;
                t[0] = 4.0f;
                t[1] = 1.0f;
                t[2] = 10.0f;
                t[3] = 16.0f;
            }
        texels[12] = texels[13] = texels[14] = texels[15] = 999.0f; // padding after the third texel of row 0
        const mt::Totals t = mt::Reduce(texels, 3, 2, 16);
        CHECK(t.region == 24.0 && t.visible == 6.0 && t.sum == 60.0 && t.taps == 96.0);
        CHECK(Near(t.Share(), 0.25, 1e-12) && Near(t.Mean(), 2.5, 1e-12) && Near(t.Coverage(), 0.25, 1e-12));
        const mt::Totals none;
        CHECK(!none.HaveRegion() && none.Share() == 0.0 && none.Mean() == 0.0 && none.Coverage() == 0.0);
    }

    // ---- the schedule -------------------------------------------------------------------------------------------------------------------------------
    {
        // twelve metered effects, sixty frames a second, twenty seconds: each effect is sampled about every three seconds, never two samples in a frame,
        // and none while one is in flight (a sample is collected two frames after it starts)
        mt::Schedule schedule;
        uint64_t inFlightUntilFrame = 0;
        bool flight = false;
        uint32_t samples[mt::kEffectCount] {};
        uint64_t firstMs[mt::kEffectCount] {};
        uint64_t lastMs[mt::kEffectCount] {};
        uint32_t perFrame = 0;
        for (uint64_t frame = 1; frame <= 20 * 60; ++frame)
        {
            const uint64_t now = 5000 + frame * 1000 / 60;
            if (flight && frame >= inFlightUntilFrame)
            {
                schedule.Collected();
                flight = false;
            }
            perFrame = 0;
            for (uint32_t i = 0; i < mt::kEffectCount; ++i)
            {
                if (!mt::kEffects[i].metered)
                    continue;
                if (schedule.Due(StageId(i), now, frame))
                {
                    schedule.Started(StageId(i), now, frame);
                    CHECK(!flight);
                    flight = true;
                    inFlightUntilFrame = frame + 2;
                    ++samples[i];
                    ++perFrame;
                    if (firstMs[i] == 0)
                        firstMs[i] = now;
                    lastMs[i] = now;
                }
            }
            CHECK(perFrame <= 1);
        }
        for (uint32_t i = 0; i < mt::kEffectCount; ++i)
        {
            if (!mt::kEffects[i].metered)
                continue;
            CHECK(samples[i] >= 5 && samples[i] <= 7); // 20 s / 3 s
        }
        // the first samples are spread: the effects do not all come in the first frame
        std::set<uint64_t> firsts;
        for (uint32_t i = 0; i < mt::kEffectCount; ++i)
            if (mt::kEffects[i].metered)
                firsts.insert(firstMs[i]);
        CHECK(firsts.size() >= 8);

        // a lost result is dropped after kMaxPendingMs and the next sample may start
        mt::Schedule lost;
        CHECK(lost.Due(StageId::ContactShadows, 1000, 1));
        lost.Started(StageId::ContactShadows, 1000, 1);
        CHECK(lost.InFlight());
        CHECK(!lost.Due(StageId::Atmosphere, 2999, 2)); // 1999 ms in flight: still waited for
        CHECK(lost.InFlight());
        CHECK(lost.Due(StageId::Atmosphere, 3750, 3)); // 2750 ms: the lost result is dropped, and Atmosphere's stagger slot (11 x 250 ms) has come
        CHECK(!lost.InFlight());
        // the same frame never starts two
        mt::Schedule same;
        same.Due(StageId::Gi, 0, 1);
        CHECK(same.Due(StageId::Ao, 100000, 9));
        same.Started(StageId::Ao, 100000, 9);
        same.Collected();
        CHECK(!same.Due(StageId::Dof, 100000, 9));
        CHECK(same.Due(StageId::Dof, 100000, 10));
    }

    // ---- the reason key -----------------------------------------------------------------------------------------------------------------------------
    {
        CHECK(mt::ReasonKey("weight 0 (night curve 0.081, moon): the engine's own picture") == mt::ReasonKey("Weight 0 (night curve 0.185, moon): the engine's own picture"));
        CHECK(mt::ReasonKey("sun at 5307.4x19.0") == "sun at #x#");
        CHECK(mt::ReasonKey("The Frame Was Left As It Was") == "the frame was left as it was");
        CHECK(mt::ReasonKey(std::string(500, 'x')).size() == mt::kReasonChars);
        CHECK(mt::ReasonKey("").empty());
    }

    // ---- the frame book -----------------------------------------------------------------------------------------------------------------------------
    {
        mt::Book book;
        // frame 1: AO ran (four reader draws), Atmosphere skipped (no camera), Gi ran after a skip in the same frame (ran beats skipped)
        for (int i = 0; i < 4; ++i)
            book.NoteRan(StageId::Ao, 1);
        book.NoteSkip(StageId::Atmosphere, 1, "the frame's camera rows never landed");
        book.NoteSkip(StageId::Gi, 1, "wait");
        book.NoteRan(StageId::Gi, 1);
        // frame 2: Ao skipped twice (one frame, one skip), Atmosphere ran
        book.NoteSkip(StageId::Ao, 2, "paused 12 frames");
        book.NoteSkip(StageId::Ao, 2, "paused 13 frames");
        book.NoteRan(StageId::Atmosphere, 2);
        // frame 4: nothing but Ao
        book.NoteRan(StageId::Ao, 4);
        // a stale event of frame 2 arrives late: ignored
        book.NoteRan(StageId::Dof, 2);
        book.FoldNow();
        CHECK_EQ(book.WindowFrames(), 3u);
        CHECK_EQ(book.Window(StageId::Ao).reached, 3u);
        CHECK_EQ(book.Window(StageId::Ao).ran, 2u);
        CHECK_EQ(book.Window(StageId::Ao).skipped, 1u);
        CHECK_EQ(book.Window(StageId::Atmosphere).reached, 2u);
        CHECK_EQ(book.Window(StageId::Atmosphere).ran, 1u);
        CHECK_EQ(book.Window(StageId::Atmosphere).skipped, 1u);
        CHECK_EQ(book.Window(StageId::Gi).reached, 1u);
        CHECK_EQ(book.Window(StageId::Gi).ran, 1u);
        CHECK_EQ(book.Window(StageId::Gi).skipped, 0u); // ran beats skipped
        CHECK_EQ(book.Window(StageId::Dof).reached, 0u);
        // the reasons: the two Ao skips share one key (digits folded), per frame once
        CHECK(std::string(book.Window(StageId::Ao).reasons[0].key) == "paused # frames" && book.Window(StageId::Ao).reasons[0].count == 1);
        CHECK(std::string(book.Window(StageId::Atmosphere).reasons[0].key) == "the frame's camera rows never landed");
        // ran % over the rendered frames: Ao 2 of 3
        CHECK(Near(mt::Percent(double(book.Window(StageId::Ao).ran), double(book.WindowFrames())), 66.6667, 0.001));
        // an effect that is off is never reached, so it reads 0 of N
        CHECK_EQ(book.Window(StageId::Shafts).reached, 0u);

        // the window closes into the total; the total includes the window being counted
        book.CloseWindow();
        CHECK_EQ(book.WindowFrames(), 0u);
        CHECK_EQ(book.TotalFrames(), 3u);
        book.NoteRan(StageId::Ao, 9);
        book.FoldNow();
        CHECK_EQ(book.Total(StageId::Ao).ran, 3u);
        CHECK_EQ(book.TotalFrames(), 4u);
        CHECK_EQ(book.Window(StageId::Ao).ran, 1u);

        // more reasons than slots: the extras are "other", the skipped count stays whole
        mt::Book many;
        for (uint64_t f = 1; f <= 20; ++f)
            many.NoteSkip(StageId::Dof, f, std::string("reason ") + char('a' + f)); // 20 different words
        many.FoldNow();
        const mt::Tally& t = many.Window(StageId::Dof);
        uint64_t keyed = 0;
        for (const mt::ReasonCount& r : t.reasons)
            keyed += r.count;
        CHECK_EQ(t.skipped, 20u);
        CHECK_EQ(keyed + t.other, 20u);
        CHECK(t.other == 20u - mt::kReasonSlots);

        // events of a frame are folded when a later token arrives, not before
        mt::Book lazy;
        lazy.NoteRan(StageId::Gi, 5);
        CHECK_EQ(lazy.WindowFrames(), 0u);
        lazy.NoteRan(StageId::Gi, 6);
        CHECK_EQ(lazy.WindowFrames(), 1u);
    }

    // ---- the clock ----------------------------------------------------------------------------------------------------------------------------------
    {
        mt::Cadence c;
        CHECK(!c.Started() && !c.WindowDue(100000) && !c.SummaryDue(100000));
        c.Tick(1000); // the first rendered frame
        CHECK(c.Started());
        CHECK(!c.WindowDue(1000 + 9999));
        CHECK(c.WindowDue(1000 + 10000));
        c.TookWindow(1000 + 10000);
        CHECK(!c.WindowDue(1000 + 10001) && c.WindowDue(1000 + 20000));
        // the summary: 60 s after the first rendered frame, then every 5 minutes
        CHECK(!c.SummaryDue(1000 + 59999));
        CHECK(c.SummaryDue(1000 + 60000));
        c.TookSummary(1000 + 60500);
        CHECK(!c.SummaryDue(1000 + 60500 + 299999));
        CHECK(c.SummaryDue(1000 + 60500 + 300000));
        CHECK_EQ(c.Summaries(), 1u);
        // later ticks do not move the start
        c.Tick(999999);
        CHECK(c.SummaryDue(1000 + 60500 + 300000));
        // a clock that went backwards starts again
        mt::Cadence back;
        back.Tick(50000);
        back.Tick(100);
        CHECK(!back.SummaryDue(100 + 59999) && back.SummaryDue(100 + 60000));
    }

    // ---- the wording --------------------------------------------------------------------------------------------------------------------------------
    {
        mt::Tally t;
        t.reached = 100;
        t.ran = 80;
        mt::Sample s;
        s.have = true;
        s.haveRegion = true;
        s.share = 0.234;
        s.mean = 11.46;
        s.coverage = 0.9;
        s.count = 4;
        // the brief's sentence
        CHECK(mt::FormatSummaryLine(StageId::Gi, 100, t, s) ==
              "Effect visibility: Light bounce ran 80 % of frames, changed 23 % of its region by > 8/255 (mean 11.5/255)");
        mt::Sample none;
        CHECK(mt::FormatSummaryLine(StageId::Atmosphere, 200, t, none) == "Effect visibility: Atmosphere ran 40 % of frames, no visibility sample yet");
        mt::Sample empty;
        empty.have = true;
        CHECK(mt::FormatSummaryLine(StageId::Ao, 100, t, empty).find("nothing in its region") != std::string::npos);
        // the card
        CHECK(mt::FormatCard(false, 100, t, s) == "Meter: off");
        CHECK(mt::FormatCard(true, 0, t, s) == "Meter: not reached yet");
        CHECK(mt::FormatCard(true, 100, mt::Tally {}, s) == "Meter: not reached yet");
        CHECK(mt::FormatCard(true, 100, t, none) == "Meter: ran 80 %, visible not measured yet");
        CHECK(mt::FormatCard(true, 100, t, s) == "Meter: ran 80 %, visible 23 % (mean 11.5/255)");
        // the effect line carries the frames, the reasons, the share and the region text
        mt::Tally withSkips = t;
        withSkips.skipped = 20;
        withSkips.AddReason("the camera never landed", 15);
        withSkips.AddReason("no fog state", 5);
        const std::string line = mt::FormatEffectLine(StageId::Atmosphere, 10100, 100, withSkips, s);
        CHECK(line.find("Effect meter: Atmosphere: in the last 10.1 s (100 rendered frames) reached 100, ran 80, skipped 20 (15 the camera never landed; 5 no fog state)") !=
              std::string::npos);
        CHECK(line.find("23.4 % of its region") != std::string::npos && line.find("> 8/255") != std::string::npos && line.find("mean 11.46/255") != std::string::npos &&
              line.find("4 samples") != std::string::npos);
        CHECK(mt::FormatEffectLine(StageId::Atmosphere, 10000, 100, t, none).find("no visibility sample yet") != std::string::npos);
    }

    return Finish("meter_rules");
}
