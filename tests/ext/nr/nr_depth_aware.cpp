// Depth-aware enlargement (DepthAwareWeights.h): the bilateral weight function, the taps, the fallbacks, the depth texel mapping, the
// shader parameter packing and the decision/status of the feature. The shader mirrors these; nr_community_shader runs it on WARP.
#include "ext/nr/DepthAwareWeights.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace
{
int failures = 0;
void Check(bool ok, const char* what, int line)
{
    if (!ok)
    {
        std::printf("FAIL line %d: %s\n", line, what);
        ++failures;
    }
}
#define CHECK(x) Check((x), #x, __LINE__)

using namespace DlssNr::DepthAware;
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

bool Near(double a, double b, double tolerance) { return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= tolerance; }
float Sum(const std::array<float, 4>& w) { return w[0] + w[1] + w[2] + w[3]; }

void ConstantsAndBits()
{
    CHECK(kSigma == 0.1f && kEpsilon == 1e-3f && kMinWeightSum == 1e-4f && kFieldCount == 28);
    // the selector bit sits above the spatial mode byte (0xff) and the foveated bit (0x100), and is a single bit
    CHECK(kResolveBit == 0x200u && (kResolveBit & 0x1ffu) == 0 && (kResolveBit & (kResolveBit - 1)) == 0);
}

void DepthValidityAndDisparity()
{
    CHECK(ValidDepth(0.0f) && ValidDepth(1.0f) && ValidDepth(0.5f) && ValidDepth(1e-7f));
    CHECK(!ValidDepth(-0.001f) && !ValidDepth(1.001f) && !ValidDepth(kNaN) && !ValidDepth(kInf) && !ValidDepth(-kInf));
    // large disparity = near, in both conventions
    CHECK(Disparity(0.8f, true) == 0.8f && Near(Disparity(0.8f, false), 0.2, 1e-6));
    CHECK(Disparity(0.9f, true) > Disparity(0.1f, true) && Disparity(0.1f, false) > Disparity(0.9f, false));
}

void SimilarityFormula()
{
    for (bool inverted : { false, true })
    {
        CHECK(Similarity(0.4f, 0.4f, inverted) == 1.0f);
        // falls as the depths move apart, in either direction
        float last = 1.0f;
        for (float step : { 0.001f, 0.003f, 0.01f, 0.03f, 0.1f })
        {
            const float up = Similarity(0.4f, 0.4f + step, inverted), down = Similarity(0.4f, 0.4f - step, inverted);
            CHECK(up < last && up > 0.0f && down > 0.0f);
            last = up;
        }
    }
    // exact value: exp(-|dp - dt| / (sigma * max(dp, eps))) on the disparities
    CHECK(Near(Similarity(0.8f, 0.7f, true), std::exp(-0.1 / (0.1 * 0.8)), 1e-5));
    CHECK(Near(Similarity(0.2f, 0.3f, false), std::exp(-0.1 / (0.1 * 0.8)), 1e-5)); // disparities 0.8 and 0.7
    // the conventions differ: the same two numbers are an edge in one reading and a mild step in the other
    CHECK(Similarity(0.9f, 0.8f, false) < 0.01f && Similarity(0.9f, 0.8f, true) > 0.2f);
    // sigma widens the tolerance, epsilon floors the divisor (sky and far plane: disparity near 0)
    CHECK(Similarity(0.5f, 0.45f, true, 0.2f) > Similarity(0.5f, 0.45f, true, 0.1f));
    // ... and both reach the weights: Weights() hands the sigma and the epsilon it was given to the similarity, not the defaults
    {
        const std::array<float, 4> bilinear { 0.4f, 0.1f, 0.4f, 0.1f };
        const std::array<float, 4> steps { 0.9f, 0.8f, 0.9f, 0.8f };
        CHECK(Weights(bilinear, steps, 0.9f, true, 0.5f)[1] > Weights(bilinear, steps, 0.9f, true, 0.1f)[1]);
        CHECK(Weights(bilinear, steps, 0.9f, true, 0.1f)[1] > Weights(bilinear, steps, 0.9f, true, 0.05f)[1]);
        CHECK(Near(Weights(bilinear, steps, 0.9f, true, 0.5f)[1], 0.083397, 1e-4)); // 0.1 e^(-0.1 / 0.45) over 0.8 + 0.2 e^(-0.1 / 0.45)
        // the pixel sits at the far plane (disparity 0); the epsilon decides how much a tiny depth difference counts
        const std::array<float, 4> faintSteps { 0.0004f, 0.002f, 0.0004f, 0.002f };
        const auto wide = Weights(bilinear, faintSteps, 0.0f, true, kSigma, 4e-3f);
        const auto narrow = Weights(bilinear, faintSteps, 0.0f, true, kSigma, 1e-3f);
        CHECK(Near(Sum(wide), 1.0, 1e-5) && Near(Sum(narrow), 1.0, 1e-5));
        CHECK(wide[1] > 10.0f * narrow[1] && wide[1] > 1e-3f && narrow[1] < 1e-6f);
        CHECK(Weights(bilinear, faintSteps, 0.0f, true, kSigma, kEpsilon) == narrow);
    }
    CHECK(Near(Similarity(0.0f, 0.0004f, true), std::exp(-0.0004 / (0.1 * 1e-3)), 1e-5));
    CHECK(Near(Similarity(0.0f, 0.0004f, true, 0.1f, 4e-3f), std::exp(-0.0004 / (0.1 * 4e-3)), 1e-5));
    CHECK(Similarity(0.0f, 0.05f, true) < 1e-6f);
    // the divisor is the pixel's own disparity, not the tap's
    CHECK(Similarity(0.8f, 0.6f, true) != Similarity(0.6f, 0.8f, true));
}

void TapsOfABilinearRead()
{
    // the centre of a texel reads only that texel
    const Taps centre = BilinearTaps(TapCentre(2, 8), TapCentre(3, 4), 8, 4);
    CHECK(centre.x[0] == 2 && centre.y[0] == 3 && Near(centre.bilinear[0], 1.0, 1e-5));
    CHECK(Near(centre.bilinear[1] + centre.bilinear[2] + centre.bilinear[3], 0.0, 1e-5));
    // midway between four texels
    const Taps mid = BilinearTaps(0.5f, 0.5f, 4, 4);
    CHECK(mid.x[0] == 1 && mid.x[1] == 2 && mid.y[0] == 1 && mid.y[2] == 2 && mid.x[2] == 1 && mid.x[3] == 2 && mid.y[1] == 1 && mid.y[3] == 2);
    for (float w : mid.bilinear)
        CHECK(Near(w, 0.25, 1e-6));
    // quarter of the way: weights follow the fraction on each axis (x fraction 0.25 -> left taps 0.75, y fraction 0.75 -> lower taps 0.75)
    const Taps quarter = BilinearTaps(TapCentre(1, 4) + 0.25f / 4, TapCentre(1, 4) + 0.75f / 4, 4, 4);
    CHECK(Near(quarter.bilinear[0], 0.75 * 0.25, 1e-5) && Near(quarter.bilinear[1], 0.25 * 0.25, 1e-5));
    CHECK(Near(quarter.bilinear[2], 0.75 * 0.75, 1e-5) && Near(quarter.bilinear[3], 0.25 * 0.75, 1e-5));
    // at the edges both taps clamp to the edge texel and the weights still sum to one
    const Taps corner = BilinearTaps(0.0f, 0.0f, 4, 4);
    CHECK(corner.x[0] == 0 && corner.x[1] == 0 && corner.y[0] == 0 && corner.y[2] == 0 && Near(corner.bilinear[0] + corner.bilinear[1] + corner.bilinear[2] + corner.bilinear[3], 1.0, 1e-6));
    const Taps far = BilinearTaps(1.0f, 1.0f, 4, 4);
    CHECK(far.x[0] == 3 && far.x[1] == 3 && far.y[0] == 3 && far.y[2] == 3);
    // x and y are not interchangeable on a non-square texture
    const Taps wide = BilinearTaps(0.9f, 0.4f, 10, 4);
    CHECK(wide.x[0] == 8 && wide.x[1] == 9 && wide.y[0] == 1 && wide.y[2] == 2);
    // the bottom edge of a non-square texture clamps to ITS last row (3), not to the width's (9), and the right edge to its last column
    const Taps tall = BilinearTaps(0.5f, 1.0f, 10, 4);
    CHECK(tall.y[0] == 3 && tall.y[1] == 3 && tall.y[2] == 3 && tall.y[3] == 3);
    const Taps right = BilinearTaps(1.0f, 0.5f, 10, 4);
    CHECK(right.x[0] == 9 && right.x[1] == 9 && right.x[2] == 9 && right.x[3] == 9 && right.y[0] == 1 && right.y[2] == 2);
    const Taps top = BilinearTaps(0.5f, 0.0f, 10, 4);
    CHECK(top.y[0] == 0 && top.y[1] == 0 && top.y[2] == 0 && top.y[3] == 0);
    // a texture of one texel in an axis reads it on both sides
    const Taps thin = BilinearTaps(0.3f, 0.7f, 1, 6);
    CHECK(thin.x[0] == 0 && thin.x[1] == 0 && thin.x[2] == 0 && thin.x[3] == 0);
    CHECK(TapCentre(0, 4) == 0.125f && TapCentre(3, 4) == 0.875f);
}

void WeightsAcrossAnEdge()
{
    const std::array<float, 4> bilinear { 0.4f, 0.1f, 0.4f, 0.1f }; // a vertical edge between the left and right taps
    for (bool inverted : { false, true })
    {
        const float nearD = inverted ? 0.8f : 0.2f, farD = inverted ? 0.1f : 0.9f;
        // a pixel on the near side: the far taps (right column) are shut out and the near ones take the whole weight
        const auto near = Weights(bilinear, { nearD, farD, nearD, farD }, nearD, inverted);
        CHECK(Near(Sum(near), 1.0, 1e-6) && near[1] < 1e-4f && near[3] < 1e-4f);
        CHECK(Near(near[0], 0.5, 1e-3) && Near(near[2], 0.5, 1e-3));
        // a pixel on the far side: the opposite
        const auto farSide = Weights(bilinear, { nearD, farD, nearD, farD }, farD, inverted);
        CHECK(Near(Sum(farSide), 1.0, 1e-6) && farSide[0] < 1e-4f && farSide[2] < 1e-4f && Near(farSide[1], 0.5, 1e-3));
    }
    // constant depth: the weights are the bilinear ones
    for (bool inverted : { false, true })
    {
        const auto same = Weights(bilinear, { 0.3f, 0.3f, 0.3f, 0.3f }, 0.3f, inverted);
        for (size_t i = 0; i < 4; ++i)
            CHECK(same[i] == bilinear[i]);
        // a gentle slope keeps most of the bilinear shape
        const auto slope = Weights(bilinear, { 0.30f, 0.302f, 0.30f, 0.302f }, 0.301f, inverted);
        CHECK(Near(Sum(slope), 1.0, 1e-6) && slope[0] > 0.3f && slope[1] > 0.05f);
    }
    // a tap the filter does not use (weight 0) stays unused
    const auto unused = Weights({ 1.0f, 0.0f, 0.0f, 0.0f }, { 0.5f, 0.5f, 0.5f, 0.5f }, 0.5f, true);
    CHECK(unused[0] == 1.0f && unused[1] == 0.0f && unused[2] == 0.0f && unused[3] == 0.0f);
    // the weights are never negative and always sum to one when they were computed
    for (float pixel : { 0.05f, 0.4f, 0.95f })
        for (float other : { 0.0f, 0.2f, 0.7f, 1.0f })
        {
            const auto w = Weights(bilinear, { pixel, other, pixel, other }, pixel, true);
            CHECK(w[0] >= 0 && w[1] >= 0 && w[2] >= 0 && w[3] >= 0 && Near(Sum(w), 1.0, 1e-5));
        }
}

void FallbackToBilinear()
{
    // not uniform: a fallback to "quarters" must not pass for the sampler's own weights
    const std::array<float, 4> bilinear { 0.4f, 0.1f, 0.4f, 0.1f };
    // an unusable depth under the pixel or under any one tap: exactly the bilinear weights
    const std::array<float, 4> good { 0.2f, 0.4f, 0.6f, 0.8f };
    for (float bad : { kNaN, kInf, -0.5f, 1.5f })
    {
        CHECK(Weights(bilinear, good, bad, true) == bilinear);
        for (size_t tap = 0; tap < 4; ++tap)
        {
            auto depths = good;
            depths[tap] = bad;
            CHECK(Weights(bilinear, depths, 0.4f, true) == bilinear);
        }
    }
    // every weight vanishing (a pixel of a thin object in front of nothing like it): bilinear again
    const auto vanished = Weights(bilinear, { 0.0f, 0.0f, 0.0f, 0.0f }, 0.9f, true, 0.001f);
    CHECK(vanished == bilinear);
    // ... but a single surviving tap is enough to be used
    const auto one = Weights(bilinear, { 0.9f, 0.0f, 0.0f, 0.0f }, 0.9f, true);
    CHECK(Near(one[0], 1.0, 1e-3) && one[1] < 1e-3f);
    // the threshold: a summed weight between zero and 1e-4 (here about 4.5e-5, with the default sigma) is "vanished" too. The taps differ,
    // so normalising them would NOT give the bilinear weights back: only the fallback does
    const std::array<float, 4> faint { 0.0f, 0.01f, 0.0f, 0.01f };
    CHECK(Weights(bilinear, faint, 0.9f, true) == bilinear);
    // a sum exactly at the threshold is still "vanished" (the weight sum must exceed it): the unnormalised bilinear weight comes back
    const auto atThreshold = Weights({ kMinWeightSum, 0.0f, 0.0f, 0.0f }, { 0.5f, 0.5f, 0.5f, 0.5f }, 0.5f, true);
    CHECK(atThreshold[0] == kMinWeightSum && atThreshold[1] == 0.0f);
    // the SUM decides, not the largest weight: four weights of 6e-5 sum to 2.4e-4 and are used, normalised
    const auto summed = Weights({ 6e-5f, 6e-5f, 6e-5f, 6e-5f }, { 0.5f, 0.5f, 0.5f, 0.5f }, 0.5f, true);
    for (float w : summed)
        CHECK(Near(w, 0.25, 1e-6));
    // just above the threshold the weights are used (normalised), not the bilinear ones
    const auto justAbove = Weights({ 1.5e-4f, 0.0f, 0.0f, 0.0f }, { 0.5f, 0.5f, 0.5f, 0.5f }, 0.5f, true);
    CHECK(Near(justAbove[0], 1.0, 1e-6));
    // a sum well above the threshold but far below 1 is used normalised as well: all four taps are far from the pixel (sum about 1.7e-3
    // with these depths: exp(-6) and exp(-7) times the quarters), and the nearer pair keeps the larger share, 0.3655 against 0.1345
    const auto mid = Weights({ 0.25f, 0.25f, 0.25f, 0.25f }, { 0.8f, 0.85f, 0.8f, 0.85f }, 0.5f, true);
    CHECK(Near(mid[0], 0.36553, 2e-3) && Near(mid[1], 0.13447, 2e-3) && Near(mid[2], mid[0], 1e-6) && Near(Sum(mid), 1.0, 1e-5));
    // a bad depth under the pixel or a tap gives back the non-uniform weights exactly (not quarters)
    CHECK(Weights(bilinear, { kNaN, 0.5f, 0.5f, 0.5f }, 0.5f, true) == bilinear && Weights(bilinear, { 0.5f, 0.5f, 0.5f, 0.5f }, kNaN, true) == bilinear);
}

void DepthTexelMapping()
{
    // a depth texture smaller than the frame, read through its valid rectangle at an offset: (3, 2) and 10 x 6 texels
    const float rect[4] = { 3, 2, 10, 6 };
    int x = -1, y = -1;
    DepthPixel(0.0f, 0.0f, rect, x, y);
    CHECK(x == 3 && y == 2);
    DepthPixel(0.5f, 0.5f, rect, x, y);
    CHECK(x == 8 && y == 5);
    DepthPixel(0.9999f, 0.9999f, rect, x, y);
    CHECK(x == 12 && y == 7);
    DepthPixel(1.0f, 1.0f, rect, x, y); // one past the end clamps to the last texel of the rectangle
    CHECK(x == 12 && y == 7);
    DepthPixel(-0.2f, 1.7f, rect, x, y);
    CHECK(x == 3 && y == 7);
    DepthPixel(0.25f, 0.75f, rect, x, y); // x = 3 + 2.5, y = 2 + 4.5
    CHECK(x == 5 && y == 6);
    // the axes are not interchangeable
    DepthPixel(0.1f, 0.9f, rect, x, y);
    CHECK(x == 4 && y == 7);
}

void ShaderParameterSlots()
{
    float fields[kFieldCount];
    for (float& f : fields)
        f = 99.0f;
    const float rect[4] = { 3, 2, 10, 6 };
    PackFields(fields, rect, true, 0.15f, 0.002f);
    CHECK(fields[0] == 3 && fields[1] == 2 && fields[2] == 10 && fields[3] == 6);
    CHECK(fields[4] == 0.15f && fields[5] == 0.002f && fields[6] == 1.0f);
    CHECK(fields[7] == kMinWeightSum); // the one threshold: the shader reads it from the slot instead of keeping a literal of its own
    for (unsigned i = 8; i < kFieldCount; ++i)
        CHECK(fields[i] == 0.0f); // nothing of an earlier use is left in the slots
    PackFields(fields, rect, false);
    CHECK(fields[4] == kSigma && fields[5] == kEpsilon && fields[6] == 0.0f && fields[7] == kMinWeightSum);
    PackFields(fields, rect, false, kSigma, kEpsilon, 3e-3f);
    CHECK(fields[7] == 3e-3f && fields[6] == 0.0f && fields[5] == kEpsilon);
}

void TheSelectorBitTravelsWithADepth()
{
    // with a depth the word is passed on as it is
    for (uint32_t word : { 0u, 1u, 2u, 0x100u, 0x102u, kResolveBit, kResolveBit | 2u, kResolveBit | 0x100u, 0xffffffffu })
        CHECK(ResolveSelector(word, true) == word);
    // without one only the depth-aware bit goes, every other bit (the mode byte, the foveated profile) stays
    CHECK(ResolveSelector(kResolveBit, false) == 0u);
    CHECK(ResolveSelector(kResolveBit | 1u, false) == 1u && ResolveSelector(kResolveBit | 2u, false) == 2u);
    CHECK(ResolveSelector(kResolveBit | 0x102u, false) == 0x102u && ResolveSelector(0x102u, false) == 0x102u && ResolveSelector(0u, false) == 0u);
    CHECK(ResolveSelector(0xffffffffu, false) == 0xfffffdffu);
    CHECK(ResolveSelector(0x7u | kResolveBit, false) == 0x7u);
}

void FeatureDecision()
{
    // the key off is off whatever else is true
    for (bool spatial : { false, true })
        for (bool enlarging : { false, true })
            for (bool depth : { false, true })
                CHECK(Decide(false, spatial, enlarging, depth) == State::Off);
    CHECK(Decide(true, false, true, true) == State::Active);
    // spatial compression wins over everything else
    CHECK(Decide(true, true, true, true) == State::InactiveSpatial && Decide(true, true, false, false) == State::InactiveSpatial);
    CHECK(Decide(true, false, false, true) == State::InactiveNotReduced && Decide(true, false, false, false) == State::InactiveNotReduced);
    CHECK(Decide(true, false, true, false) == State::InactiveNoDepth);
    CHECK(std::strcmp(StatusText(State::InactiveSpatial), "depth-aware resolve: inactive with spatial compression") == 0);
    CHECK(std::strcmp(StatusText(State::Active), "depth-aware resolve: active") == 0);
    CHECK(std::strcmp(StatusText(State::InactiveNotReduced), "depth-aware resolve: inactive at model resolution 100%") == 0);
    CHECK(std::strcmp(StatusText(State::InactiveAboveFull), "depth-aware resolve: inactive: model resolution above 100% on an axis") == 0);
    CHECK(std::strcmp(StatusText(State::InactiveNoDepth), "depth-aware resolve: inactive (no usable depth)") == 0);
    CHECK(std::strcmp(StatusText(State::Off), "") == 0);
    // a model above 100% on an axis is brought back down by the supersample filter: nothing is enlarged, and the status says why
    CHECK(Decide(true, false, false, true, true) == State::InactiveAboveFull && Decide(true, false, false, false, true) == State::InactiveAboveFull);
    CHECK(Decide(true, false, false, true, false) == State::InactiveNotReduced);
    CHECK(Decide(true, true, false, true, true) == State::InactiveSpatial);              // spatial still wins
    CHECK(Decide(false, false, false, true, true) == State::Off);                         // and the key off is off
    CHECK(Decide(true, false, true, true, true) == State::Active && Decide(true, false, true, false, true) == State::InactiveNoDepth);
    // what counts as enlarging: reduced on some axis and not enlarged on any
    CHECK(Enlarging(960, 540, 1920, 1080, false) && Enlarging(960, 1080, 1920, 1080, false) && Enlarging(1920, 540, 1920, 1080, false));
    CHECK(!Enlarging(1920, 1080, 1920, 1080, false) && !Enlarging(960, 540, 1920, 1080, true) && !Enlarging(2880, 1620, 1920, 1080, true));
    CHECK(!Enlarging(1920, 1080, 1920, 1080, true));
    // the width is compared with the frame's width, not its height: 1500 is below 1920 but not below 1080
    CHECK(Enlarging(1500, 1080, 1920, 1080, false) && Enlarging(1080, 1920, 1080, 2160, false) && !Enlarging(1920, 1080, 1080, 1080, false));
    CHECK(!Enlarging(1080, 1080, 1080, 1080, false) && Enlarging(1079, 1080, 1080, 1080, false) && Enlarging(1080, 1079, 1080, 1080, false));
}
} // namespace

int main()
{
    ConstantsAndBits();
    DepthValidityAndDisparity();
    SimilarityFormula();
    TapsOfABilinearRead();
    WeightsAcrossAnEdge();
    FallbackToBilinear();
    DepthTexelMapping();
    ShaderParameterSlots();
    TheSelectorBitTravelsWithADepth();
    FeatureDecision();
    if (failures != 0)
    {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("PASS: depth-aware enlargement (weight formula, taps, fallbacks, depth texel mapping, shader slots, decision and status)");
    return 0;
}
