// [VFXMASK] The rules of the VFX mask gate probe (ext/ffxiv/vfxmask/VfxMaskCore.h): the key's values, which columns a half
// covers, the size guard, the log gate and the log text. Pure CPU, no D3D and no NGX type, so every rule the product applies is
// checked here without a device.
//
// The half rule is the one the in-game measurement rests on: one masked half and one unmasked half in the same picture, so the
// unmasked half can be used as the noise floor of the on-versus-off difference.
#include "ext/ffxiv/vfxmask/VfxMaskCore.h"

#include <cstdio>
#include <string>

using namespace ext::ffxiv::vfxmask;

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

static bool Contains(const std::string& text, const std::string& needle)
{
    return text.find(needle) != std::string::npos;
}

int main()
{
    // ---- the key: three values, anything else is off ----------------------------------------------------------------
    CHECK(ParseMode("off") == ProbeMode::Off);
    CHECK(ParseMode("left") == ProbeMode::Left);
    CHECK(ParseMode("right") == ProbeMode::Right);
    CHECK(ParseMode("LEFT") == ProbeMode::Left);
    CHECK(ParseMode("Right") == ProbeMode::Right);
    CHECK(ParseMode("  left\t") == ProbeMode::Left);   // surrounding spaces and tabs
    CHECK(ParseMode("") == ProbeMode::Off);
    CHECK(ParseMode("auto") == ProbeMode::Off);        // the ini's unset value
    CHECK(ParseMode("lef") == ProbeMode::Off);
    CHECK(ParseMode("left half") == ProbeMode::Off);   // a value the build does not know, never a guess
    CHECK(ParseMode("1") == ProbeMode::Off);           // numbers are not modes
    CHECK(ParseMode(std::string_view("right\0x", 7)) == ProbeMode::Off);

    CHECK(std::string(ModeName(ProbeMode::Off)) == "off");
    CHECK(std::string(ModeName(ProbeMode::Left)) == "left");
    CHECK(std::string(ModeName(ProbeMode::Right)) == "right");

    // ---- the half: the two modes are exact complements, also for an odd width --------------------------------------
    for (unsigned width = 2; width <= 9; ++width)
    {
        for (unsigned x = 0; x < width; ++x)
        {
            CHECK(MaskedAt(x, width, ProbeMode::Left) != MaskedAt(x, width, ProbeMode::Right));
            CHECK(MaskedAt(x, width, ProbeMode::Off) == false);
        }
    }
    // Even width: each half is exactly half of the row.
    CHECK(MaskedAt(0, 64, ProbeMode::Left) && MaskedAt(31, 64, ProbeMode::Left) && !MaskedAt(32, 64, ProbeMode::Left) &&
          !MaskedAt(63, 64, ProbeMode::Left));
    CHECK(!MaskedAt(0, 64, ProbeMode::Right) && !MaskedAt(31, 64, ProbeMode::Right) && MaskedAt(32, 64, ProbeMode::Right) &&
          MaskedAt(63, 64, ProbeMode::Right));
    // Odd width: the right half is the wider one by a column, and the middle column belongs to it alone.
    CHECK(MaskedAt(1, 5, ProbeMode::Left) && !MaskedAt(2, 5, ProbeMode::Left));
    CHECK(!MaskedAt(1, 5, ProbeMode::Right) && MaskedAt(2, 5, ProbeMode::Right) && MaskedAt(4, 5, ProbeMode::Right));
    // A real frame: the left half of 3840 is the left half of the render window at every quality preset.
    CHECK(MaskedAt(1919, 3840, ProbeMode::Left) && !MaskedAt(1920, 3840, ProbeMode::Left));
    // Out of the row, or a row too narrow to split: nothing is masked (fail open).
    CHECK(!MaskedAt(64, 64, ProbeMode::Left) && !MaskedAt(0, 1, ProbeMode::Left) && !MaskedAt(0, 0, ProbeMode::Right));

    // ---- the values written: 1.0 inside the masked half, 0.0 outside -------------------------------------------------
    CHECK(MaskValueAt(0, 8, ProbeMode::Left) == 255 && MaskValueAt(7, 8, ProbeMode::Left) == 0);
    CHECK(MaskValueAt(0, 8, ProbeMode::Right) == 0 && MaskValueAt(7, 8, ProbeMode::Right) == 255);

    for (unsigned width : { 2u, 3u, 8u, 3839u })
    {
        std::string row(width, '\x01');
        FillRow(reinterpret_cast<std::uint8_t*>(row.data()), width, ProbeMode::Left);
        unsigned masked = 0;
        for (unsigned x = 0; x < width; ++x)
        {
            const bool inside = static_cast<unsigned char>(row[x]) == kMaskValue;
            CHECK(inside == MaskedAt(x, width, ProbeMode::Left));
            masked += inside ? 1 : 0;
        }
        CHECK(masked == width / 2); // the left half is floor(width / 2) columns, at every width
    }
    {
        std::string row(8, '\x01');
        FillRow(reinterpret_cast<std::uint8_t*>(row.data()), 8, ProbeMode::Right);
        CHECK(std::string(row.begin(), row.begin() + 4) == std::string(4, '\x00')); // cleared
        CHECK(std::string(row.begin() + 4, row.end()) == std::string(4, static_cast<char>(kMaskValue)));
    }
    FillRow(nullptr, 8, ProbeMode::Left); // no row, no crash

    // ---- the size guard ---------------------------------------------------------------------------------------------
    {
        const MaskPlan ok = PlanMask(3840, 2160, ProbeMode::Left);
        CHECK(ok.ok && ok.width == 3840 && ok.height == 2160);
        CHECK(PlanMask(2, 1, ProbeMode::Right).ok);       // the smallest row that can be split
        CHECK(PlanMask(kMaxMaskEdge, kMaxMaskEdge, ProbeMode::Left).ok);
    }
    CHECK(!PlanMask(1, 1, ProbeMode::Left).ok);           // one column cannot be split
    CHECK(!PlanMask(0, 2160, ProbeMode::Left).ok);
    CHECK(!PlanMask(3840, 0, ProbeMode::Left).ok);
    CHECK(!PlanMask(kMaxMaskEdge + 1, 2160, ProbeMode::Left).ok);
    CHECK(!PlanMask(3840, kMaxMaskEdge + 1, ProbeMode::Left).ok);
    CHECK(!PlanMask(3840, 2160, ProbeMode::Off).ok);      // the key off never plans a mask

    // ---- the log gate: one line per change, a budget per session, a re-arm says so again -----------------------------
    {
        LogGate gate;
        const LogKey given { ProbeMode::Left, 3840, 2160, Outcome::Given, 1 };
        unsigned taken = 0;
        taken += gate.Take(given) ? 1 : 0;       // the first evaluate says what it did
        taken += gate.Take(given) ? 1 : 0;       // the next frames say nothing
        taken += gate.Take({ ProbeMode::Left, 3840, 2160, Outcome::Unconfirmed, 1 }) ? 1 : 0; // a block that stopped keeping it
        taken += gate.Take({ ProbeMode::Right, 3840, 2160, Outcome::Given, 1 }) ? 1 : 0;      // the other half
        taken += gate.Take({ ProbeMode::Right, 1920, 1080, Outcome::Given, 1 }) ? 1 : 0;      // a resolution change
        taken += gate.Take({ ProbeMode::Right, 1920, 1080, Outcome::Refused, 1 }) ? 1 : 0;    // it stopped working
        taken += gate.Take({ ProbeMode::Right, 1920, 1080, Outcome::Given, 1 }) ? 1 : 0;      // back
        taken += gate.Take({ ProbeMode::Right, 1920, 1080, Outcome::Given, 2 }) ? 1 : 0;      // switched off and on again
        taken += gate.Take({ ProbeMode::Right, 1920, 1080, Outcome::Given, 2 }) ? 1 : 0;      // and quiet again
        CHECK(taken == 7);

        // A game that keeps changing the situation cannot flood the log: 32 lines a session, then nothing.
        LogGate budget;
        unsigned written = 0;
        for (unsigned i = 0; i < LogGate::kMaxLines + 10; ++i)
            written += budget.Take({ ProbeMode::Left, 100 + i, 50, Outcome::Refused, i }) ? 1 : 0;
        CHECK(written == LogGate::kMaxLines);
        CHECK(!budget.Take({ ProbeMode::Left, 999, 50, Outcome::Given, 99 }));
    }

    // ---- the line itself --------------------------------------------------------------------------------------------
    {
        const std::string given = FormatLine({ ProbeMode::Left, 3840, 2160, Outcome::Given, 1 }, "DLSS", nullptr);
        CHECK(Contains(given, "left half"));
        CHECK(Contains(given, "3840x2160"));
        CHECK(Contains(given, "R8_UNORM"));
        CHECK(Contains(given, "DLSS"));
        CHECK(Contains(given, "reports the mask back"));
        CHECK(!Contains(given, "does not report"));

        const std::string unconfirmed = FormatLine({ ProbeMode::Right, 1920, 1080, Outcome::Unconfirmed, 1 }, "XeSS", nullptr);
        CHECK(Contains(unconfirmed, "right half"));
        CHECK(Contains(unconfirmed, "XeSS"));
        CHECK(Contains(unconfirmed, "does not report the mask back"));

        const std::string refused =
            FormatLine({ ProbeMode::Left, 3840, 0, Outcome::Refused, 1 }, "FSR", "the colour input has no size");
        CHECK(Contains(refused, "nothing written"));
        CHECK(Contains(refused, "the colour input has no size"));
        CHECK(Contains(refused, "3840x0"));
        CHECK(!Contains(refused, "R8_UNORM"));

        // A missing reason and a missing backend name never produce a hole in the line.
        const std::string bare = FormatLine({ ProbeMode::Left, 2, 1, Outcome::Refused, 1 }, nullptr, nullptr);
        CHECK(Contains(bare, "unknown reason"));
        const std::string noBackend = FormatLine({ ProbeMode::Left, 2, 1, Outcome::Given, 1 }, nullptr, nullptr);
        CHECK(Contains(noBackend, "unknown bias-mask input"));
    }

    if (g_failed != 0)
    {
        std::printf("%d check(s) failed\n", g_failed);
        return 1;
    }
    std::puts("PASS: VFX mask probe rules: key values, exact-complement halves, size guard, log gate and text");
    return 0;
}
