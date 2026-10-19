// [MIP] The source policy of the automatic mipmap bias (ext/upscaler/MipBias.h): "follow the latest accepted feature
// create". Replays the sessions that were logged as fake event traces and pins the decisions the glue acts on and the
// log lines the in-game check reads. Pure CPU, no upstream types.
//
// The traces use the numbers of the six logged FFXIV sessions: the game starts in native DLAA at 3840x2160 (login
// screen), the forced Quality request moves it to 2560x1440 about a minute later, the menu can then go to Balanced
// (2258x1270) and back to DLAA.
#include "ext/upscaler/MipBias.h"

#include <cmath>
#include <cstdio>
#include <string>

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

#define CHECK_TEXT(actual, expected)                                                                             \
    do                                                                                                           \
    {                                                                                                            \
        const std::string a_ = (actual);                                                                         \
        const std::string e_ = (expected);                                                                       \
        if (a_ != e_)                                                                                            \
        {                                                                                                        \
            std::printf("FAIL line %d:\n  got      %s\n  expected %s\n", __LINE__, a_.c_str(), e_.c_str());     \
            ++g_failed;                                                                                          \
        }                                                                                                        \
    } while (0)

static bool Near(double a, double b, double tolerance = 1e-6) { return std::fabs(a - b) <= tolerance; }

// One game: the policy plus a stand-in for what the sampler hooks would read. Mirrors what the glue does with an
// Outcome (the real thing is tested in mip_bias_glue with a fake optional).
struct Game
{
    Policy policy;
    bool overrideEngaged = false;
    double overrideValue = 0.0;

    Outcome Create(unsigned w, unsigned h, unsigned outW, unsigned outH, bool extended = false, double os = 1.0)
    {
        return Act(OnCreate(policy, w, h, outW, outH, extended, os));
    }
    Outcome Config(bool enabled, double offset) { return Act(Configure(policy, enabled, offset)); }

  private:
    Outcome Act(const Outcome& o)
    {
        if (o.action == Action::Engage || o.action == Action::Publish)
        {
            overrideEngaged = true;
            overrideValue = o.value;
        }
        else if (o.action == Action::Restore)
            overrideEngaged = false;
        return o;
    }
};

int main()
{
    // ---- the logged session, offset 0, as an event trace --------------------------------------------------------
    {
        Game g;

        // Config load with auto on: the override is engaged at once and adds nothing yet. The first engagement is not
        // subject to the 0.001 threshold (the last value was "nothing", which also reads as 0).
        auto o = g.Config(true, 0.0);
        CHECK(o.action == Action::Engage);
        CHECK(o.value == 0.0 && o.changed);
        CHECK(g.overrideEngaged && g.overrideValue == 0.0);

        // Login screen: native DLAA, render == display. Accepted, nothing moves.
        o = g.Create(3840, 2160, 3840, 2160);
        CHECK(o.accepted && o.reject == Reject::None && o.log);
        CHECK(o.action == Action::Publish && o.value == 0.0 && !o.changed && o.was == 0.0);
        CHECK(Near(o.ratio, 1.0));
        CHECK_TEXT(CreateLine(o),
                   "Automatic mipmap bias: create render 3840x2160, display 3840x2160, ratio 1.000 -> bias 0.0000 "
                   "(unchanged, offset +0.00)");

        // The forced Quality request lands: the game recreates its feature at 2560x1440.
        o = g.Create(2560, 1440, 3840, 2160);
        CHECK(o.accepted && o.changed && o.log);
        CHECK(Near(o.value, -0.584963) && o.was == 0.0);
        CHECK(Near(g.overrideValue, -0.584963));
        CHECK_TEXT(CreateLine(o),
                   "Automatic mipmap bias: create render 2560x1440, display 3840x2160, ratio 1.500 -> bias -0.5850 "
                   "(was 0.0000, offset +0.00); applies to samplers created from now on");

        // The same size again (FFXIV recreates its feature at several points): unchanged, and it is still logged.
        o = g.Create(2560, 1440, 3840, 2160);
        CHECK(o.accepted && !o.changed && o.log && Near(o.value, -0.584963) && Near(o.was, -0.584963));
        CHECK_TEXT(CreateLine(o),
                   "Automatic mipmap bias: create render 2560x1440, display 3840x2160, ratio 1.500 -> bias -0.5850 "
                   "(unchanged, offset +0.00)");

        // The preview is a different create entirely (576x960 -> 1152x1920, a portrait display): ignored, the value
        // stays, and the line says why.
        o = g.Create(576, 960, 1152, 1920);
        CHECK(!o.accepted && o.reject == Reject::Portrait && o.log && o.action == Action::None);
        CHECK(Near(g.overrideValue, -0.584963));
        CHECK_TEXT(IgnoredLine(o), std::string("Automatic mipmap bias: ignored create 576x960 -> 1152x1920: ") +
                                       RejectText(Reject::Portrait));

        // Menu switch to Balanced: the exact number he types by hand today.
        o = g.Create(2258, 1270, 3840, 2160);
        CHECK(o.changed && Near(o.value, -0.766061) && Near(o.was, -0.584963));
        CHECK(Near(o.ratio, 3840.0 / 2258.0));

        // Back to native DLAA: the bias goes back to 0.
        o = g.Create(3840, 2160, 3840, 2160);
        CHECK(o.changed && o.value == 0.0 && Near(o.was, -0.766061));
        CHECK(g.overrideValue == 0.0);
    }

    // ---- a player who forces Quality but plays at native DLAA all session ---------------------------------------
    // The forced preset is not an input of this policy on purpose (that is what rules out the "forced preset wins"
    // variant): every create is 3840x2160 for the whole session, so the bias is 0 for the whole session.
    {
        Game g;
        g.Config(true, 0.0);
        for (int i = 0; i < 5; ++i)
        {
            const auto o = g.Create(3840, 2160, 3840, 2160);
            CHECK(o.accepted && o.value == 0.0 && !o.changed);
            CHECK(g.overrideValue == 0.0);
        }
    }

    // ---- ignored creates never move anything --------------------------------------------------------------------
    {
        Game g;
        g.Config(true, 0.0);
        g.Create(2560, 1440, 3840, 2160);

        // Zero sizes, sizes out of range, a tiny display: all ignored, with a different reason each.
        auto o = g.Create(0, 0, 3840, 2160);
        CHECK(!o.accepted && o.reject == Reject::OutOfRange && o.log);
        CHECK_TEXT(IgnoredLine(o), std::string("Automatic mipmap bias: ignored create 0x0 -> 3840x2160: ") +
                                       RejectText(Reject::OutOfRange));
        o = g.Create(300, 200, 600, 400);
        CHECK(!o.accepted && o.reject == Reject::TinyDisplay);
        CHECK(Near(g.overrideValue, -0.584963));

        // An ignored create does not replace the remembered sizes: turning the feature off and on again uses the
        // last ACCEPTED create.
        g.Config(false, 0.0);
        o = g.Config(true, 0.0);
        CHECK(o.action == Action::Engage && Near(o.value, -0.584963));
    }

    // ---- render larger than display (extended limits) -------------------------------------------------------------
    {
        Game g;
        g.Config(true, 0.0);
        g.Create(2560, 1440, 3840, 2160);
        auto o = g.Create(5760, 3240, 3840, 2160, true);
        CHECK(o.accepted && o.value == 0.0 && o.changed && Near(o.was, -0.584963));
        CHECK(Near(o.ratio, 3840.0 / 5760.0));
        // Without extended limits the same raw numbers are a swapped pair: render 3840, display 5760.
        o = g.Create(5760, 3240, 3840, 2160, false);
        CHECK(o.accepted && Near(o.value, std::log2(3840.0 / 5760.0)));
        // 6x upscaling with extended limits.
        o = g.Create(640, 360, 3840, 2160, true);
        CHECK(Near(o.value, -2.584963));
    }

    // ---- the offset ---------------------------------------------------------------------------------------------
    {
        Game g;
        // Offset -0.18 on the Quality trace gives back his old look to within 0.001.
        auto o = g.Config(true, -0.18);
        CHECK(o.action == Action::Engage && Near(o.value, -0.18));         // before any create: the offset alone
        o = g.Create(2560, 1440, 3840, 2160);
        CHECK(Near(o.value, -0.764963));
        CHECK(std::fabs(o.value - (-0.766061)) < 0.0012);
        CHECK_TEXT(CreateLine(o),
                   "Automatic mipmap bias: create render 2560x1440, display 3840x2160, ratio 1.500 -> bias -0.7650 "
                   "(was -0.1800, offset -0.18); applies to samplers created from now on");

        // Changing the offset re-publishes from the last accepted sizes (not from 0).
        o = g.Config(true, 0.5);
        CHECK(o.action == Action::Publish && o.changed && Near(o.value, -0.584963 + 0.5) && Near(o.was, -0.764963));
        CHECK(Near(g.overrideValue, -0.084963));

        // Out-of-range offsets are limited to +-3.
        o = g.Config(true, 10.0);
        CHECK(Near(o.value, -0.584963 + 3.0) && o.offset == 3.0);
        o = g.Config(true, -10.0);
        CHECK(Near(o.value, -0.584963 - 3.0) && o.offset == -3.0);
    }

    // ---- output scaling ---------------------------------------------------------------------------------------------
    {
        Game g;
        g.Config(true, 0.0);
        auto o = g.Create(2560, 1440, 3840, 2160, false, 1.5);
        CHECK(Near(o.value, -1.169925) && Near(o.ratio, 2.25) && o.outputScaling == 1.5);
        CHECK_TEXT(CreateLine(o),
                   "Automatic mipmap bias: create render 2560x1440, display 3840x2160 (x1.50 output scaling), ratio "
                   "2.250 -> bias -1.1699 (was 0.0000, offset +0.00); applies to samplers created from now on");
        // Re-enabling after an off period keeps the output scaling of the create it follows.
        g.Config(false, 0.0);
        o = g.Config(true, 0.0);
        CHECK(Near(o.value, -1.169925));
    }

    // ---- hysteresis: a create that differs by less than 0.001 is "unchanged" -------------------------------------
    {
        Game g;
        g.Config(true, 0.0);
        auto o = g.Create(2560, 1440, 3840, 2160);
        // 2561 / 3840 differs from 2560 / 3840 by 0.00056 in bias.
        o = g.Create(2561, 1440, 3840, 2160);
        CHECK(o.accepted && !o.changed && Near(o.value, -0.584963) && Near(o.was, -0.584963));
        CHECK(Near(g.overrideValue, -0.584963));
        // 2570 differs by 0.0056: that moves.
        o = g.Create(2570, 1445, 3840, 2160);
        CHECK(o.changed && Near(o.value, std::log2(2570.0 / 3840.0)));
    }

    // ---- idempotent configuration -------------------------------------------------------------------------------
    {
        // The config load runs again on every Config::Reload: it must republish from the last accepted sizes, never 0.
        Game g;
        g.Config(true, 0.0);
        g.Create(2560, 1440, 3840, 2160);
        auto o = g.Config(true, 0.0);
        CHECK(o.action == Action::Publish && !o.changed && Near(o.value, -0.584963));
        CHECK(Near(g.overrideValue, -0.584963));
    }

    // ---- auto off ------------------------------------------------------------------------------------------------
    {
        Game g;
        // Never engaged: configuring "off" does nothing at all.
        auto o = g.Config(false, 0.0);
        CHECK(o.action == Action::None && !g.overrideEngaged);

        // Off at create time: sizes are remembered silently (no log line, no action) ...
        o = g.Create(2560, 1440, 3840, 2160);
        CHECK(o.accepted && o.action == Action::None && !o.log && !g.overrideEngaged);
        o = g.Create(576, 960, 1152, 1920);
        CHECK(!o.accepted && !o.log && o.action == Action::None); // ... and an ignored one is silent too

        // ... so ticking the checkbox mid-session applies the current ratio straight away.
        o = g.Config(true, 0.0);
        CHECK(o.action == Action::Engage && Near(o.value, -0.584963) && o.haveSizes);
        CHECK_TEXT(SettingLine(o, true),
                   "Automatic mipmap bias: on, offset +0.00, bias -0.5850 from the last create (render 2560x1440, "
                   "display 3840x2160)");

        // Unticking restores what the user had.
        o = g.Config(false, 0.0);
        CHECK(o.action == Action::Restore && !g.overrideEngaged);
        CHECK_TEXT(SettingLine(o, false), "Automatic mipmap bias: off, the manual setting is back in force");

        // After that creates are silent again and a second "off" is a no-op.
        o = g.Create(1920, 1080, 3840, 2160);
        CHECK(o.accepted && o.action == Action::None && !o.log);
        o = g.Config(false, 0.0);
        CHECK(o.action == Action::None);
    }

    // ---- "configured" tells the glue whether the config load reached the policy ---------------------------------------
    {
        Policy fresh;
        CHECK(!fresh.configured);
        OnCreate(fresh, 2560, 1440, 3840, 2160, false, 1.0); // a create alone never configures (the glue catches up)
        CHECK(!fresh.configured && !fresh.enabled && fresh.haveSizes);
        Configure(fresh, false, 0.0);
        CHECK(fresh.configured);
    }

    // ---- on before any create ---------------------------------------------------------------------------------------
    {
        Game g;
        auto o = g.Config(true, 0.25);
        CHECK(!o.haveSizes);
        CHECK_TEXT(SettingLine(o, true),
                   "Automatic mipmap bias: on, offset +0.25, bias 0.2500 until the next upscaler create");
    }

    // ---- the menu texts ----------------------------------------------------------------------------------------------
    {
        Game g;
        g.Config(true, 0.0);
        auto s = StatusOf(g.policy);
        CHECK(s.enabled && !s.haveSizes);
        CHECK_TEXT(StatusLine(s), "No upscaler created yet: bias 0.000 until then, offset +0.00.");

        g.Create(2560, 1440, 3840, 2160);
        s = StatusOf(g.policy);
        CHECK(s.enabled && s.haveSizes && Near(s.bias, -0.584963) && Near(s.ratio, 1.5));
        CHECK_TEXT(StatusLine(s), "Ratio 1.50x (2560x1440 to 3840x2160): bias -0.585, offset +0.00.");

        g.Create(2560, 1440, 3840, 2160, false, 1.5);
        CHECK_TEXT(StatusLine(StatusOf(g.policy)),
                   "Ratio 2.25x (2560x1440 to 3840x2160, output scaling x1.50): bias -1.170, offset +0.00.");

        g.Config(false, 0.0);
        CHECK(!StatusOf(g.policy).enabled);
    }
    CHECK_TEXT(CreatedLine(100.0f, -100.0f), "No texture sampler created since the last change.");
    CHECK_TEXT(CreatedLine(-2.33f, -0.58f), "Created since the last change: min -2.33 / max -0.58");

    // A value that rounds to zero never prints as "-0.0000" (an offset of -0.00003 is what puts one in "was").
    {
        Game g;
        g.Config(true, -0.00003);
        const auto o = g.Create(2560, 1440, 3840, 2160);
        CHECK_TEXT(CreateLine(o),
                   "Automatic mipmap bias: create render 2560x1440, display 3840x2160, ratio 1.500 -> bias -0.5850 "
                   "(was 0.0000, offset +0.00); applies to samplers created from now on");
    }

    // ---- the Scale override ("MB Scale Override") blocks auto: Scale multiplies the game's negative bias by the negative
    //      automatic value, which blurs. No Engage, no Publish, one warning per episode. -----------------------------------
    {
        Policy p;
        Outcome o = Configure(p, true, 0.0, true); // config load: auto on, Scale on
        CHECK(o.action == Action::None && !p.engaged);
        CHECK(o.blocked && o.warn && p.warned);

        o = Configure(p, true, 0.0, true); // a second load or a slider release in the same episode: no second warning
        CHECK(o.action == Action::None && o.blocked && !o.warn);

        // creates while blocked: the sizes are remembered, nothing is engaged, published or logged
        o = OnCreate(p, 2560, 1440, 3840, 2160, false, 1.0, true);
        CHECK(o.accepted && o.action == Action::None && !o.log && !o.warn && !p.engaged && p.haveSizes);
        o = OnCreate(p, 1280, 720, 3840, 2160, false, 1.0, true);
        CHECK(o.action == Action::None && !p.engaged && !o.changed);
        CHECK(StatusOf(p).blocked && StatusOf(p).bias == 0.0);

        // Scale override unticked: the next create (or the card) engages from the remembered sizes at once
        o = OnCreate(p, 2560, 1440, 3840, 2160, false, 1.0, false);
        CHECK(o.action == Action::Engage && o.log && Near(o.value, -0.584963) && !o.blocked && !o.warn);
        CHECK(!StatusOf(p).blocked);

        // ticked again while engaged: the override is given back and the new episode warns once
        o = OnCreate(p, 2560, 1440, 3840, 2160, false, 1.0, true);
        CHECK(o.action == Action::Restore && o.warn && o.blocked && !p.engaged && !o.log);
        o = OnCreate(p, 2560, 1440, 3840, 2160, false, 1.0, true);
        CHECK(o.action == Action::None && !o.warn);

        // the same flip seen by a create that is itself ignored (the preview): still given back / taken again
        o = OnCreate(p, 576, 960, 1152, 1920, false, 1.0, false);
        CHECK(!o.accepted && o.action == Action::Engage && Near(o.value, -0.584963));
        o = OnCreate(p, 576, 960, 1152, 1920, false, 1.0, true);
        CHECK(!o.accepted && o.action == Action::Restore && o.warn);

        // auto switched off while Scale is on: nothing to warn about, nothing engaged
        o = Configure(p, false, 0.0, true);
        CHECK(o.action == Action::None && !o.blocked && !o.warn && !StatusOf(p).blocked);
        // ... and on again with Scale still on: a new episode
        o = Configure(p, true, 0.0, true);
        CHECK(o.blocked && o.warn);
    }
    {
        // Scale on from the start of the session: native DLAA login screen then the world: never engaged
        Policy p;
        Configure(p, true, 0.0, true);
        for (const unsigned render : { 3840u, 2560u, 2258u })
        {
            const Outcome o = OnCreate(p, render, render * 9 / 16, 3840, 2160, false, 1.0, true);
            CHECK(o.action == Action::None && !p.engaged);
        }
        CHECK(StatusOf(p).bias == 0.0);
    }
    {
        // texts
        CHECK(BlockedLine(false).empty());
        CHECK(BlockedLine(true).find("utomatic bias needs additive mode; MB Scale Override is on") != std::string::npos);
        CHECK(ScaleModeWarning().find("MB Scale Override") != std::string::npos);
        const std::string note = CardNote();
        CHECK(note.find("Fixed replaces the game's own per-sampler biases with the single value") != std::string::npos);
        CHECK(note.find("Turn it off from the menu while no level is loading, or edit the ini") != std::string::npos);
        CHECK(note.find("portrait") != std::string::npos);
        CHECK(note.find("\"Set\" and \"Reset\"") != std::string::npos);
    }

    // ---- OnRawCreate: the whole create path the glue runs (output scaling choice, extended limits) -------------------------
    {
        RawCreate c;
        c.width = 2560;
        c.height = 1440;
        c.outWidth = 3840;
        c.outHeight = 2160;
        c.outputScalingEnabled = true;
        c.outputScalingMultiplier = 1.5;

        {   // output scaling on, full-res motion vectors, render < display: the upscaler outputs at display size, no multiplier
            Policy p;
            Configure(p, true, 0.0);
            const Outcome o = OnRawCreate(p, c);
            CHECK(o.accepted && o.outputScaling == 1.0);
            CHECK(Near(o.value, std::log2(2560.0 / 3840.0)));           // -0.585, not -1.170
        }
        {   // the same with low-res motion vectors: the output really is 1.5x the display
            Policy p;
            Configure(p, true, 0.0);
            c.lowResMv = true;
            const Outcome o = OnRawCreate(p, c);
            CHECK(o.outputScaling == 1.5);
            CHECK(Near(o.value, std::log2(2560.0 / (3840.0 * 1.5))));   // -1.170
            c.lowResMv = false;
        }
        {   // render == display: the upscaler scales its output whatever the vector flag says
            Policy p;
            Configure(p, true, 0.0);
            RawCreate d = c;
            d.width = 3840;
            d.height = 2160;
            const Outcome o = OnRawCreate(p, d);
            CHECK(o.outputScaling == 1.5);
            CHECK(Near(o.value, std::log2(1.0 / 1.5)));
        }
        {   // option off: the multiplier is ignored whatever the flags are
            Policy p;
            Configure(p, true, 0.0);
            RawCreate d = c;
            d.outputScalingEnabled = false;
            d.lowResMv = true;
            const Outcome o = OnRawCreate(p, d);
            CHECK(o.outputScaling == 1.0);
            CHECK(Near(o.value, std::log2(2560.0 / 3840.0)));
        }
        {   // extended limits are forwarded unchanged: Width/Height stay the render size even when they exceed the display
            RawCreate d;
            d.width = 5000;
            d.height = 2800;
            d.outWidth = 3840;
            d.outHeight = 2160;
            Policy normal;
            const Outcome swapped = OnRawCreate(normal, d);
            CHECK(swapped.accepted && swapped.sizes.renderW == 3840 && swapped.sizes.displayW == 5000);

            d.extendedLimits = true;
            Policy extended;
            const Outcome as_given = OnRawCreate(extended, d);
            CHECK(as_given.accepted && as_given.sizes.renderW == 5000 && as_given.sizes.displayW == 3840);
        }
        {   // a create that is rejected changes nothing
            Policy p;
            Configure(p, true, 0.0);
            RawCreate d = c;
            d.outWidth = 0;
            const Outcome o = OnRawCreate(p, d);
            CHECK(!o.accepted && o.reject == Reject::OutOfRange && !p.haveSizes);
        }
    }

    if (g_failed != 0)
    {
        std::printf("%d check(s) failed\n", g_failed);
        return 1;
    }

    std::puts("PASS: automatic mip bias policy: follows the latest accepted create, replay of the logged session, "
              "ignored creates, offset, auto off/on");
    return 0;
}
