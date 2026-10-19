// [MIP] How an Outcome of the automatic mip bias policy is written to the override (ext/upscaler/MipBias.h, Apply):
// engaged while auto is on, the user's ini value kept aside and given back, no write below 0.001, and the two
// upstream menu buttons ("Set" and "Reset") that can touch the value while auto is on.
//
// The real override is upstream's CustomOptional<float, NoDefault> (Config.h), which pulls in Config.h / State.h and
// is not linked into the tests. FakeOverride below mirrors the members Apply uses, copied from CustomOptional's
// behaviour (Config.h: set_volatile_value, set_from_config, the operator= overloads that clear the volatile flag,
// value_for_config); reset() is std::optional's own and, like the real one, does NOT clear the volatile flag.
#include "ext/upscaler/MipBias.h"

#include <cmath>
#include <cstdio>
#include <optional>

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

struct FakeOverride : std::optional<float>
{
    std::optional<float> configIni;
    bool isVolatile = false;
    int volatileWrites = 0; // test bookkeeping, not part of CustomOptional

    void set_volatile_value(const float& value)
    {
        ++volatileWrites;
        if (!isVolatile)
        { // make sure the previously set value is saved
            if (this->has_value())
                configIni = this->value();
            else
                configIni = std::nullopt;
        }
        isVolatile = true;
        std::optional<float>::operator=(value);
    }

    void set_from_config(const std::optional<float>& opt)
    {
        if (!this->has_value())
        {
            configIni = opt;
            std::optional<float>::operator=(opt);
        }
    }

    FakeOverride& operator=(const float& value)
    {
        isVolatile = false;
        std::optional<float>::operator=(value);
        return *this;
    }
    FakeOverride& operator=(float&& value)
    {
        isVolatile = false;
        std::optional<float>::operator=(value);
        return *this;
    }
    FakeOverride& operator=(const std::optional<float>& opt)
    {
        isVolatile = false;
        std::optional<float>::operator=(opt);
        return *this;
    }
    FakeOverride& operator=(std::optional<float>&& opt)
    {
        isVolatile = false;
        std::optional<float>::operator=(std::move(opt));
        return *this;
    }

    std::optional<float> value_for_config()
    {
        if (isVolatile)
            return configIni;
        if (this->has_value())
            return this->value();
        return std::nullopt;
    }
};

static Outcome Engage(double value)
{
    Outcome o;
    o.action = Action::Engage;
    o.value = value;
    return o;
}
static Outcome Publish(double value)
{
    Outcome o;
    o.action = Action::Publish;
    o.value = value;
    return o;
}
static Outcome Restore()
{
    Outcome o;
    o.action = Action::Restore;
    return o;
}

int main()
{
    // ---- the user has a manual value in the ini (-0.766061, his current setting) ------------------------------------
    {
        FakeOverride ov;
        ov.set_from_config(-0.766061f); // what Config::Reload does with the ini

        CHECK(Apply(ov, Engage(0.0)));
        CHECK(ov.has_value() && ov.value() == 0.0f);             // the hooks now see the policy's value
        CHECK(ov.isVolatile);
        CHECK(Near(*ov.value_for_config(), -0.766061));          // and Save() still writes the user's value

        CHECK(Apply(ov, Publish(-0.584963)));
        CHECK(Near(ov.value(), -0.584963));
        CHECK(Near(*ov.value_for_config(), -0.766061));          // still the user's

        // The config load runs again on Config::Reload: a second Engage must not take the policy's value for the user's.
        CHECK(Apply(ov, Engage(-0.584963)));
        CHECK(Near(*ov.value_for_config(), -0.766061));

        // Auto off: the user's value is back, not volatile, and what Save() would write.
        CHECK(Apply(ov, Restore()));
        CHECK(ov.has_value() && Near(ov.value(), -0.766061));
        CHECK(!ov.isVolatile);
        CHECK(Near(*ov.value_for_config(), -0.766061));

        // on -> off -> on -> off keeps the user's value through every round.
        CHECK(Apply(ov, Engage(-1.0)));
        CHECK(Near(ov.value(), -1.0) && Near(*ov.value_for_config(), -0.766061));
        CHECK(Apply(ov, Restore()));
        CHECK(Near(ov.value(), -0.766061) && !ov.isVolatile);
    }

    // ---- the user has no manual value --------------------------------------------------------------------------------
    {
        FakeOverride ov;
        ov.set_from_config(std::nullopt);
        CHECK(!ov.has_value());

        CHECK(Apply(ov, Engage(0.0)));
        CHECK(ov.has_value() && ov.value() == 0.0f);             // engaged at 0: the hooks track samplers from the start
        CHECK(!ov.value_for_config().has_value());               // Save() still writes "auto"

        CHECK(Apply(ov, Restore()));
        CHECK(!ov.has_value() && !ov.isVolatile);                // empty again, volatile flag cleared (reset() would not)
        CHECK(!ov.value_for_config().has_value());
    }

    // ---- a manual value that happens to equal the engaged one is still taken over and given back -------------------------
    {
        FakeOverride ov;
        ov.set_from_config(0.0f);
        CHECK(Apply(ov, Engage(0.0)));
        CHECK(ov.volatileWrites == 1 && ov.isVolatile);          // the first engagement bypasses the threshold
        CHECK(Apply(ov, Publish(-0.5)));
        CHECK(Apply(ov, Restore()));
        CHECK(ov.has_value() && ov.value() == 0.0f);
    }

    // ---- no write below 0.001 -----------------------------------------------------------------------------------------
    {
        FakeOverride ov;
        Apply(ov, Engage(-0.584963));
        const int writes = ov.volatileWrites;
        CHECK(!Apply(ov, Publish(-0.584963)));
        CHECK(!Apply(ov, Publish(-0.5854)));
        CHECK(!Apply(ov, Publish(-0.5846)));
        CHECK(ov.volatileWrites == writes);
        CHECK(Apply(ov, Publish(-0.5875)));                      // 0.0025 away
        CHECK(ov.volatileWrites == writes + 1 && Near(ov.value(), -0.5875, 1e-5));
    }

    // ---- an outcome with nothing to do ----------------------------------------------------------------------------------
    {
        FakeOverride ov;
        ov.set_from_config(-0.3f);
        Outcome none;
        CHECK(!Apply(ov, none));
        CHECK(Near(ov.value(), -0.3) && !ov.isVolatile && ov.volatileWrites == 0);
    }

    // ---- the upstream card's "Reset" while auto is on: the override is emptied, the next publish fills it again -----------
    {
        FakeOverride ov;
        ov.set_from_config(-0.766061f);
        Apply(ov, Engage(0.0));
        Apply(ov, Publish(-0.584963));

        ov.reset(); // upstream "Reset": std::optional::reset, the volatile flag stays set
        CHECK(!ov.has_value());
        CHECK(Apply(ov, Publish(-0.584963)));                    // same value as before, but the override was empty
        CHECK(ov.has_value() && Near(ov.value(), -0.584963));
        CHECK(Near(*ov.value_for_config(), -0.766061));          // the user's ini value was not lost by the Reset
        CHECK(Apply(ov, Restore()));
        CHECK(Near(ov.value(), -0.766061));
    }
    {
        // Same with no manual value in the ini: Restore gives back "none".
        FakeOverride ov;
        Apply(ov, Engage(0.0));
        ov.reset();
        CHECK(Apply(ov, Publish(0.0)));
        CHECK(ov.has_value());
        CHECK(Apply(ov, Restore()));
        CHECK(!ov.has_value());
    }

    // ---- the upstream card's "Set" while auto is on: it stores a manual value (clears the volatile flag) ------------------
    {
        FakeOverride ov;
        ov.set_from_config(-0.766061f);
        Apply(ov, Engage(0.0));
        Apply(ov, Publish(-0.584963));

        ov = 0.5f; // "Set": CustomOptional::operator=(T) -> volatile cleared
        CHECK(!ov.isVolatile && Near(ov.value(), 0.5));

        // The next quality change overwrites it, as the card says ...
        CHECK(Apply(ov, Publish(-0.766061)));
        CHECK(Near(ov.value(), -0.766061) && ov.isVolatile);
        // ... and the value the user set by hand is what is kept aside now (it is what "Set" meant to save).
        CHECK(Near(*ov.value_for_config(), 0.5));
        CHECK(Apply(ov, Restore()));
        CHECK(Near(ov.value(), 0.5));
    }
    {
        // "Set" and then auto goes off before any other publish: the manual value stays.
        FakeOverride ov;
        ov.set_from_config(-0.766061f);
        Apply(ov, Engage(0.0));
        ov = 0.25f;
        CHECK(Apply(ov, Restore()));
        CHECK(Near(ov.value(), 0.25) && !ov.isVolatile);
    }

    // ---- the policy and the override together: the logged session, with his ini value in place -------------------------
    {
        FakeOverride ov;
        ov.set_from_config(-0.7f);
        Policy policy;

        Apply(ov, Configure(policy, true, 0.0));                                   // config load
        CHECK(ov.has_value() && ov.value() == 0.0f);
        Apply(ov, OnCreate(policy, 3840, 2160, 3840, 2160, false, 1.0));           // login screen
        CHECK(ov.value() == 0.0f);
        Apply(ov, OnCreate(policy, 2560, 1440, 3840, 2160, false, 1.0));           // forced Quality
        CHECK(Near(ov.value(), -0.584963));
        Apply(ov, OnCreate(policy, 576, 960, 1152, 1920, false, 1.0));             // the preview: ignored
        CHECK(Near(ov.value(), -0.584963));
        Apply(ov, OnCreate(policy, 2258, 1270, 3840, 2160, false, 1.0));           // Balanced
        CHECK(Near(ov.value(), -0.766061, 1e-6));
        Apply(ov, Configure(policy, true, 0.0));                                   // a second config load: no jump to 0
        CHECK(Near(ov.value(), -0.766061, 1e-6));
        Apply(ov, Configure(policy, false, 0.0));                                  // untick
        CHECK(Near(ov.value(), -0.7) && !ov.isVolatile);
    }

    // ---- Scale override on: the policy never touches the override; ticking it mid-session gives the user's value back -----
    {
        FakeOverride ov;
        ov.set_from_config(-0.7f);
        Policy policy;

        Apply(ov, Configure(policy, true, 0.0, true));                                       // config load, Scale on
        CHECK(Near(ov.value(), -0.7) && !ov.isVolatile && ov.volatileWrites == 0);           // the user's value, untouched
        Apply(ov, OnCreate(policy, 2560, 1440, 3840, 2160, false, 1.0, true));
        CHECK(Near(ov.value(), -0.7) && !ov.isVolatile && ov.volatileWrites == 0);

        Apply(ov, OnCreate(policy, 2560, 1440, 3840, 2160, false, 1.0, false));              // Scale unticked
        CHECK(Near(ov.value(), -0.584963) && ov.isVolatile);
        Apply(ov, OnCreate(policy, 2560, 1440, 3840, 2160, false, 1.0, true));               // ticked again
        CHECK(Near(ov.value(), -0.7) && !ov.isVolatile);                                     // given back
        const int writes = ov.volatileWrites;
        Apply(ov, OnCreate(policy, 2258, 1270, 3840, 2160, false, 1.0, true));
        CHECK(ov.volatileWrites == writes && Near(ov.value(), -0.7));
    }

    if (g_failed != 0)
    {
        std::printf("%d check(s) failed\n", g_failed);
        return 1;
    }

    std::puts("PASS: automatic mip bias glue: engaged while on, ini value kept and restored, no write below 0.001, "
              "Set/Reset while on");
    return 0;
}
