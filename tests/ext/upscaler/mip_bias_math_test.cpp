// [MIP] The numbers of the automatic mipmap bias (ext/upscaler/MipBias.h): the formula, the create sanity guard, the
// offset and the 0.001 hysteresis. Pure CPU, no upstream types: standalone like forced_quality_math_test.cpp.
//
// Every expected value below was computed independently of the code under test (awk, log(x)/log(2)) from the numbers
// the menu's mipmap calculator uses today: render = uint(display / ratio), bias = log2(render / display).
#include "ext/upscaler/MipBias.h"

#include <cmath>
#include <cstdio>

using namespace ext::upscaler::mipbias;

static int g_failed = 0;

#define CHECK(cond)                                                                                              \
    do                                                                                                           \
    {                                                                                                            \
        if (!(cond))                                                                                             \
        {                                                                                                        \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                                                  \
            ++g_failed;                                                                                          \
        }                                                                                                        \
    } while (0)

static bool Near(double a, double b, double tolerance = 1e-6) { return std::fabs(a - b) <= tolerance; }

// What the feature would log for a create, through the same two steps the glue uses.
static double BiasOf(unsigned width, unsigned height, unsigned outWidth, unsigned outHeight, bool extended = false,
                     double outputScaling = 1.0)
{
    const auto create = CreateSizes(width, height, outWidth, outHeight, extended);
    CHECK(create.reject == Reject::None);
    return BiasFromSizes(create.sizes.renderW, create.sizes.displayW, outputScaling);
}

int main()
{
    // ---- continuity first: the maintainer's manual number --------------------------------------------------------
    // Balanced at 3840: uint(3840 / 1.7) = 2258, log2(2258 / 3840) = -0.766061 (his MipmapBiasOverride today).
    CHECK(Near(BiasOf(2258, 1270, 3840, 2160), -0.766061));
    // The plain log2(1 / 1.7) = -0.76553 is NOT the target: the integer render width is part of the formula.
    CHECK(!Near(BiasOf(2258, 1270, 3840, 2160), -0.76553, 1e-5));

    // ---- the presets at 3840 --------------------------------------------------------------------------------------
    CHECK(Near(BiasOf(2560, 1440, 3840, 2160), -0.584963));  // Quality 1.5
    CHECK(Near(BiasOf(1920, 1080, 3840, 2160), -1.0));       // Performance 2.0
    CHECK(Near(BiasOf(1280, 720, 3840, 2160), -1.584963));   // Ultra Performance 3.0
    CHECK(Near(BiasOf(2953, 1661, 3840, 2160), -0.378925));  // Ultra Quality 1.3
    CHECK(Near(BiasOf(2227, 1252, 3840, 2160), -0.786005));  // NVIDIA-style Balanced 0.58
    CHECK(Near(BiasOf(2844, 1600, 3840, 2160), -0.433185));  // ratio 1.35

    // Another display: the bias depends on the ratio, not on the pixel count.
    CHECK(Near(BiasOf(1706, 960, 2560, 1440), -0.585526));   // Quality at 2560

    // The smallest display the guard accepts still works: 1280x720 Ultra Performance (render 426).
    CHECK(Near(BiasOf(426, 240, 1280, 720), std::log2(426.0 / 1280.0)));

    // ---- output scaling: the calculator multiplies the display by the multiplier ---------------------------------
    CHECK(Near(BiasOf(2560, 1440, 3840, 2160, false, 1.5), -1.169925));
    CHECK(Near(BiasFromSizes(2560, 3840, 1.5), BiasFromSizes(2560, 5760))); // same as a display of 5760
    CHECK(Near(BiasFromSizes(2560, 3840, 1.0), -0.584963));
    // A multiplier that makes no sense (zero, negative, NaN) is treated as 1: the bias never goes through NaN/inf.
    CHECK(Near(BiasFromSizes(2560, 3840, 0.0), -0.584963));
    CHECK(Near(BiasFromSizes(2560, 3840, -2.0), -0.584963));
    CHECK(Near(BiasFromSizes(2560, 3840, std::nan("")), -0.584963));

    // ---- extended limits: 6x upscaling is -2.585, and more pixels than the output give 0 -------------------------
    CHECK(Near(BiasOf(640, 360, 3840, 2160, true), -2.584963));
    CHECK(BiasFromSizes(3840, 3840) == 0.0);   // render == display
    CHECK(BiasFromSizes(5760, 3840) == 0.0);   // render > display: no negative bias for extra pixels
    CHECK(BiasFromSizes(2560, 0) == 0.0);      // nothing to divide by
    CHECK(BiasFromSizes(0, 3840) == 0.0);      // not a render size
    CHECK(!std::signbit(BiasFromSizes(3840, 3840))); // +0, never -0 (the log prints it)

    // ---- CreateSizes mirrors IFeature::SetInitParameters ---------------------------------------------------------
    {
        const auto c = CreateSizes(2258, 1270, 3840, 2160, false);
        CHECK(c.reject == Reject::None);
        CHECK(c.sizes.renderW == 2258 && c.sizes.renderH == 1270 && c.sizes.displayW == 3840 && c.sizes.displayH == 2160);
    }
    {
        // Width/Height and OutWidth/OutHeight swapped by the game (the NGX naming trap): without extended limits the
        // smaller pair is the render size, per axis -- exactly SetInitParameters' min/max.
        const auto c = CreateSizes(3840, 2160, 2258, 1270, false);
        CHECK(c.reject == Reject::None);
        CHECK(c.sizes.renderW == 2258 && c.sizes.renderH == 1270 && c.sizes.displayW == 3840 && c.sizes.displayH == 2160);
    }
    {
        // Per axis, not per pair: one axis larger on each side.
        const auto c = CreateSizes(3840, 1270, 2258, 2160, false);
        CHECK(c.reject == Reject::None);
        CHECK(c.sizes.renderW == 2258 && c.sizes.renderH == 1270 && c.sizes.displayW == 3840 && c.sizes.displayH == 2160);
    }
    {
        // Extended limits: no swap, render may be larger than display (it is the user's explicit choice).
        const auto c = CreateSizes(5760, 3240, 3840, 2160, true);
        CHECK(c.reject == Reject::None);
        CHECK(c.sizes.renderW == 5760 && c.sizes.renderH == 3240 && c.sizes.displayW == 3840 && c.sizes.displayH == 2160);
        CHECK(BiasFromSizes(c.sizes.renderW, c.sizes.displayW) == 0.0);
    }

    // ---- the sanity guard ------------------------------------------------------------------------------------------
    // Zero or unreadable sizes (NGX parameters absent).
    CHECK(CreateSizes(0, 0, 3840, 2160, false).reject == Reject::OutOfRange);
    CHECK(CreateSizes(2560, 1440, 0, 0, false).reject == Reject::OutOfRange);
    CHECK(CreateSizes(0, 0, 0, 0, true).reject == Reject::OutOfRange);
    // SetInitParameters zeroes anything under 20 or over 16384 before it uses the sizes; the guard sees the same.
    CHECK(CreateSizes(19, 1440, 3840, 2160, false).reject == Reject::OutOfRange);
    CHECK(CreateSizes(2560, 19, 3840, 2160, false).reject == Reject::OutOfRange);
    CHECK(CreateSizes(2560, 1440, 16385, 2160, false).reject == Reject::OutOfRange);
    CHECK(CreateSizes(2560, 1440, 3840, 16385, false).reject == Reject::OutOfRange);
    CHECK(CreateSizes(16385, 1440, 3840, 2160, true).reject == Reject::OutOfRange);
    CHECK(CreateSizes(20, 20, 3840, 2160, false).reject == Reject::None); // 20 is allowed (an extreme, not a fault)
    CHECK(CreateSizes(2560, 1440, 16384, 9216, false).reject == Reject::None);

    // A display narrower than 640 is not a game scene.
    CHECK(CreateSizes(300, 200, 639, 400, false).reject == Reject::TinyDisplay);
    CHECK(CreateSizes(320, 200, 640, 400, false).reject == Reject::None);

    // The CharaView preview: 576x960 -> 1152x1920. It does not reach SetInitParameters today (checked in a game log),
    // but a create like it must not move the bias if that ever changes: a portrait display is not the main scene.
    CHECK(CreateSizes(576, 960, 1152, 1920, false).reject == Reject::Portrait);
    CHECK(CreateSizes(1152, 1920, 576, 960, false).reject == Reject::Portrait); // swapped by the game
    CHECK(CreateSizes(576, 960, 1152, 1920, true).reject == Reject::Portrait);
    // Landscape, square and ultra-wide are all scenes.
    CHECK(CreateSizes(1440, 1440, 1920, 1920, false).reject == Reject::None);
    CHECK(CreateSizes(2580, 1080, 3440, 1440, false).reject == Reject::None);
    CHECK(CreateSizes(1024, 819, 1280, 1024, false).reject == Reject::None); // 5:4 landscape
    // When two rules apply, the order is stable: range, then tiny, then portrait.
    CHECK(CreateSizes(300, 400, 600, 800, false).reject == Reject::TinyDisplay);
    CHECK(CreateSizes(0, 400, 600, 800, false).reject == Reject::OutOfRange);
    // A rejected create hands back no sizes to use.
    CHECK(CreateSizes(576, 960, 1152, 1920, false).sizes.renderW == 0);

    // Every reject has a reason text (the log line must say why).
    CHECK(RejectText(Reject::OutOfRange)[0] != '\0');
    CHECK(RejectText(Reject::TinyDisplay)[0] != '\0');
    CHECK(RejectText(Reject::Portrait)[0] != '\0');

    // ---- the offset and the final value ---------------------------------------------------------------------------
    CHECK(Near(FinalBias(BiasFromSizes(2560, 3840), -0.18), -0.764963));    // his old look on the Quality trace
    CHECK(Near(FinalBias(0.0, 0.5), 0.5));
    CHECK(Near(FinalBias(-0.584963, 0.584963), 0.0));
    // The upstream config reload resets any value outside +-15, so the final value never leaves that range.
    CHECK(FinalBias(-14.9, -0.5) == -15.0);
    CHECK(FinalBias(14.9, 0.5) == 15.0);
    CHECK(FinalBias(-100.0, 0.0) == -15.0);
    CHECK(FinalBias(0.0, 0.0) == 0.0 && !std::signbit(FinalBias(0.0, -0.0)));
    // The offset key is limited to +-3 and a NaN counts as 0.
    CHECK(ClampOffset(5.0) == 3.0);
    CHECK(ClampOffset(-5.0) == -3.0);
    CHECK(ClampOffset(-0.18) == -0.18);
    CHECK(ClampOffset(std::nan("")) == 0.0);

    // ---- hysteresis: no new value, no "changed" ---------------------------------------------------------------
    CHECK(!ShouldPublish(0.0, 0.0));
    CHECK(!ShouldPublish(-0.584963, -0.584963));
    CHECK(!ShouldPublish(0.0, 0.0005));
    CHECK(ShouldPublish(0.0, 0.001));          // the boundary itself publishes (>=, and kMinChange is 0.001)
    CHECK(!ShouldPublish(0.0, 0.00099));       // just under it does not
    CHECK(ShouldPublish(0.0, -0.001) && !ShouldPublish(0.0, -0.00099));
    CHECK(!ShouldPublish(0.0005, 0.0));
    CHECK(!ShouldPublish(-0.766061, -0.7665));
    CHECK(ShouldPublish(0.0, 0.002));
    CHECK(ShouldPublish(0.002, 0.0));
    CHECK(ShouldPublish(-0.584963, -0.766061));
    CHECK(ShouldPublish(0.0, -0.584963));
    CHECK(!ShouldPublish(0.0, std::nan("")));  // a broken value is never published
    CHECK(ShouldPublish(std::nan(""), 0.0));   // ... and a broken old value is replaced by the next good one

    if (g_failed != 0)
    {
        std::printf("%d check(s) failed\n", g_failed);
        return 1;
    }

    std::puts("PASS: automatic mip bias math: presets, output scaling, create sanity guard, offset clamp, hysteresis");
    return 0;
}
