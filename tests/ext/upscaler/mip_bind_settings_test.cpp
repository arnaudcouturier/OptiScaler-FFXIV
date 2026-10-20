// [MIP-BIND] Settings and text of the bind-time mip bias (ext/upscaler/MipBind.h): which setting wins (the key, its alias,
// the game default), that a manual value always wins over the automatic one, the MipmapBiasObserve log and the lines the
// in-game check reads.
#include "ext/upscaler/MipBind.h"

#include <cstdio>
#include <string>

using namespace ext::upscaler::mipbind;

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

static bool Has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

int main()
{
    // ---- which setting wins ------------------------------------------------------------------------------------------
    CHECK(EffectiveAuto(std::nullopt, std::nullopt, true));   // FFXIV: on by default
    CHECK(!EffectiveAuto(std::nullopt, std::nullopt, false)); // every other game: off by default
    CHECK(!EffectiveAuto(false, std::nullopt, true));         // the key off turns the FFXIV default off
    CHECK(EffectiveAuto(true, std::nullopt, false));
    CHECK(EffectiveAuto(std::nullopt, true, false));          // the alias works like the key
    CHECK(!EffectiveAuto(std::nullopt, false, true));
    CHECK(!EffectiveAuto(false, true, true));                 // the key wins over the alias
    CHECK(EffectiveAuto(true, false, false));

    // ---- a manual value always wins ----------------------------------------------------------------------------------
    CHECK(BindShouldRun(true, false));
    CHECK(!BindShouldRun(true, true));
    CHECK(!BindShouldRun(false, false));
    CHECK(!BindShouldRun(false, true));

    // ---- the observe log ---------------------------------------------------------------------------------------------
    {
        Observer o;
        const auto a = o.Note(0.0, -1.0f);
        CHECK(a && Has(*a, "original sampler bias -1.0000") && Has(*a, "1 distinct"));
        CHECK(!o.Note(0.0, -1.0f));                       // a value seen again at the same ratio: no line
        CHECK(!o.Note(0.003, -1.005f));                   // same ratio within rounding, same value within 0.02
        const auto b = o.Note(0.0, -0.5f);
        CHECK(b && Has(*b, "2 distinct"));
        const auto c = o.Note(-0.584963, -1.0f);          // the same value at another ratio is a new line
        CHECK(c && Has(*c, "ratio bias -0.5850") && Has(*c, "1 distinct"));
        CHECK(!o.Note(std::nan(""), -1.0f) && !o.Note(0.0, std::nanf("")));
        // the number of ratios is bounded
        Observer many;
        int lines = 0;
        for (int i = 0; i < 20; ++i)
            lines += many.Note(-0.1 * i, -1.0f).has_value() ? 1 : 0;
        CHECK(lines == 8);
        o.Reset();
        CHECK(o.Note(0.0, -1.0f).has_value());
    }

    // ---- lines -------------------------------------------------------------------------------------------------------
    {
        const auto line = TargetLine(2560, 3840, -0.584963, -0.18, -0.764963, Mode::Add, true);
        CHECK(Has(line, "render width 2560") && Has(line, "output width 3840") && Has(line, "ratio bias -0.5850"));
        CHECK(Has(line, "offset -0.18") && Has(line, "target -0.7650") && Has(line, "add mode") && Has(line, "first measurement"));
        const auto later = TargetLine(3840, 3840, 0.0, 0.0, 0.0, Mode::Replace, false);
        CHECK(Has(later, "target 0.0000") && Has(later, "replace mode") && !Has(later, "first measurement"));
        CHECK(!Has(later, "-0.0000"));

        Decision rep;
        rep.decided = true;
        rep.mode = Mode::Replace;
        rep.delta = -0.584963;
        rep.seenBefore = -1.0f;
        rep.seenAfter = -1.5625f;
        const auto r = DecisionLine(rep);
        CHECK(Has(r, "REPLACES") && Has(r, "-1.0000") && Has(r, "-1.5625") && Has(r, "-0.5850"));
        Decision add;
        add.decided = true;
        add.mode = Mode::Add;
        add.delta = -0.584963;
        add.created = 40;
        const auto a = DecisionLine(add);
        CHECK(Has(a, "ADDED") && Has(a, "40 new samplers") && Has(a, "art values"));
    }
    {
        BindStatus s;
        CHECK(Has(BindStatusLine(s), "not active"));
        s.installed = true;
        s.manualWins = true;
        CHECK(Has(BindStatusLine(s), "manual"));
        s.manualWins = false;
        CHECK(Has(BindStatusLine(s), "Waiting"));
        s.hasTarget = true;
        s.target = -0.585;
        s.samplers = 27;
        s.substitutedBinds = 1234;
        s.variants = 9;
        const auto line = BindStatusLine(s);
        CHECK(Has(line, "Target -0.5850") && Has(line, "add mode") && Has(line, "not decided yet") && Has(line, "27 samplers") &&
              Has(line, "1234 binds") && Has(line, "9 variants") && !Has(line, "table full") && !Has(line, "failures"));
        s.decided = true;
        s.mode = Mode::Replace;
        s.tableFull = 3;
        s.failures = 2;
        const auto line2 = BindStatusLine(s);
        CHECK(Has(line2, "replace mode") && !Has(line2, "not decided") && Has(line2, "table full 3 times") &&
              Has(line2, "2 variant failures"));
    }

    if (g_failed == 0)
        std::printf("mip_bind_settings: all passed\n");
    return g_failed == 0 ? 0 : 1;
}
