// F4 "Include OptiScaler menu": the decision of when a screenshot is taken after the menu was drawn.
#include "ext/hdr/screenshot/MenuCapture.h"

#include <cstdio>
#include <cstdlib>

using ext::hdr::screenshot::MenuCaptureInputs;
using ext::hdr::screenshot::WantsMenuCapture;

#define CHECK(cond)                                                                                              \
    do                                                                                                           \
    {                                                                                                            \
        if (!(cond))                                                                                             \
        {                                                                                                        \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond);                                         \
            return 1;                                                                                            \
        }                                                                                                        \
    } while (0)

int main()
{
    // Nothing set: the clean capture.
    CHECK(!WantsMenuCapture({}));

    // Exactly the five conditions together.
    const MenuCaptureInputs all { true, 1, true, true, true };
    CHECK(WantsMenuCapture(all));

    // Dropping any one condition falls back to the clean capture.
    { auto in = all; in.hdrActive = false;   CHECK(!WantsMenuCapture(in)); }
    { auto in = all; in.format = 0;          CHECK(!WantsMenuCapture(in)); } // SDR PNG is unchanged
    { auto in = all; in.includeMenu = false; CHECK(!WantsMenuCapture(in)); }
    { auto in = all; in.overlayMenu = false; CHECK(!WantsMenuCapture(in)); } // "Requires the overlay menu"
    { auto in = all; in.menuVisible = false; CHECK(!WantsMenuCapture(in)); } // "With the menu hidden ... clean capture"

    // Only format 1 is HDR PNG.
    for (int format = -2; format <= 3; ++format)
    {
        auto in = all;
        in.format = format;
        CHECK(WantsMenuCapture(in) == (format == 1));
    }

    // Exhaustive: of the 32 flag combinations (format 1) exactly one wants the menu capture.
    int wanted = 0;
    for (int bits = 0; bits < 32; ++bits)
    {
        const MenuCaptureInputs in { (bits & 1) != 0, (bits & 2) ? 1 : 0, (bits & 4) != 0, (bits & 8) != 0, (bits & 16) != 0 };
        if (WantsMenuCapture(in))
            ++wanted;
    }
    CHECK(wanted == 1);

    std::puts("PASS: menu capture needs OptiHDR + HDR PNG + ScreenshotMenu + overlay menu + menu visible; every other combination keeps the clean capture");
    return 0;
}
