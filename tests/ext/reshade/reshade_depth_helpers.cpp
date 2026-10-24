// Automatic ReShade scene depth: the pure helpers around the binder (capture freshness, macro value parsing, the log rule,
// the late-registration planner and the event list). No backend, no ReShade.
#include "ext/reshade/ReShadeDepthCore.h"
#include "ext/reshade/ReShadeRegister.h"

#include <cstdio>
#include <string>

using namespace ext::reshade;
using namespace ext::reshade::depth;

namespace
{
int g_fail = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            std::printf("CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #c);                                           \
            ++g_fail;                                                                                                  \
        }                                                                                                              \
    } while (0)

void Freshness()
{
    DepthCapture c;
    c.valid = true;
    c.tickMs = 5000;
    c.frame = 70;
    // age: 250 ms is fresh, 251 ms is stale
    CHECK(IsFresh(c, 5000, 70));
    CHECK(IsFresh(c, 5249, 70));
    CHECK(IsFresh(c, 5250, 70));
    CHECK(!IsFresh(c, 5251, 70));
    CHECK(!IsFresh(c, 9000, 70));
    // frames: 0 and 1 behind are fresh, 2 behind and the future are not
    CHECK(IsFresh(c, 5000, 71));
    CHECK(!IsFresh(c, 5000, 72));
    CHECK(!IsFresh(c, 5000, 69));
    CHECK(!IsFresh(c, 5000, 0));
    CHECK(!IsFresh(c, 5000, 1000));
    // a capture from the future in time is stale, too
    CHECK(!IsFresh(c, 4999, 70));
    CHECK(!IsFresh(c, 0, 70));
    // both limits apply together
    CHECK(!IsFresh(c, 5251, 71));
    CHECK(!IsFresh(c, 5250, 72));
    CHECK(IsFresh(c, 5250, 71));
    // not valid
    c.valid = false;
    CHECK(!IsFresh(c, 5000, 70));
    CHECK(!IsFresh(DepthCapture {}, 0, 0));
    // a frame counter near the top of the range stays correct
    DepthCapture w;
    w.valid = true;
    w.frame = 0x7FFFFFFFFFFFFFFEull;
    CHECK(IsFresh(w, 0, 0x7FFFFFFFFFFFFFFFull));
    static_assert(kMaxCaptureAgeMs == 250 && kMaxCaptureFrames == 1);
}

void NeutralDefinition()
{
    const char* const one[] = { "1",    "1.0",  "1.0f",      "1.0F",  " 1 ",     "\t1\t",  "\r\n1\r\n", "1.",
                                "1.000000", "01", "1e0", "10e-1", "0.1e1", "1f", "1F", "  1.0f  " };
    for (const char* v : one)
    {
        if (!IsNeutralDefinition(v, 1.0))
            std::printf("  '%s' should be neutral for 1\n", v);
        CHECK(IsNeutralDefinition(v, 1.0));
    }
    const char* const zero[] = { "0", "0.0", "0.0f", "-0", "-0.0", " 0 ", "0e5", "000", "0.", ".0", "0F" };
    for (const char* v : zero)
    {
        if (!IsNeutralDefinition(v, 0.0))
            std::printf("  '%s' should be neutral for 0\n", v);
        CHECK(IsNeutralDefinition(v, 0.0));
    }
    const char* const notOne[] = { "",     " ",    "\t",  "0",    "2",   "-1",   "0.5",  "1.0001", "0.9999999", "abc", "1.0x",
                                   "1,0",  "1 2",  "1.0 f", "ff", "f",   "1ff",  "+1",   "inf",    "-inf",      "nan", "infinity",
                                   "0x1",  "(1)",  "1;",  "1.0.0", "1e", "e1",   "--1",  "10",     "1e1" };
    for (const char* v : notOne)
    {
        if (IsNeutralDefinition(v, 1.0))
            std::printf("  '%s' should not be neutral for 1\n", v);
        CHECK(!IsNeutralDefinition(v, 1.0));
    }
    const char* const notZero[] = { "", " ", "1", "0.001", "-0.001", "1e-300", "abc", "0 0", "inf", "-inf", "nan", "0x0", "+0", "0;", "f" };
    for (const char* v : notZero)
    {
        if (IsNeutralDefinition(v, 0.0))
            std::printf("  '%s' should not be neutral for 0\n", v);
        CHECK(!IsNeutralDefinition(v, 0.0));
    }
    // the view keeps its length: text after an embedded NUL is junk, not ignored; text outside the view is not read
    CHECK(!IsNeutralDefinition(std::string_view("1\0x", 3), 1.0));
    CHECK(IsNeutralDefinition(std::string_view("1xyz", 1), 1.0));
}

void LogRule()
{
    CHECK(!ShouldLog(0));
    CHECK(ShouldLog(1));
    for (uint64_t n = 2; n < 1000; ++n)
        CHECK(!ShouldLog(n));
    CHECK(ShouldLog(1000));
    CHECK(!ShouldLog(1001));
    CHECK(!ShouldLog(1999));
    CHECK(ShouldLog(2000));
    CHECK(!ShouldLog(2500));
    CHECK(ShouldLog(1'000'000));
    CHECK(!ShouldLog(1'000'001));
}

void Names()
{
    CHECK(std::string(CounterName(Counter::OverwrittenByOther)) == "overwrittenByOther");
    CHECK(std::string(CounterName(Counter::StaleCaptures)) == "staleCaptures");
    CHECK(std::string(CounterName(Counter::SizeMismatches)) == "sizeMismatches");
    CHECK(std::string(CounterName(Counter::ManualTransformStandDowns)) == "manualTransformStandDowns");
    CHECK(std::string(CounterName(Counter::NoDepthInput)) == "noDepthInput");
    CHECK(std::string(CounterName(Counter::PassesBound)) == "passesBound");
    CHECK(std::string(CounterName(Counter::OtherDevice)) == "otherDevice");
    static_assert(kCounters == 7);
    static_assert(kConsumerWindowMs == 1000);

    CHECK(std::string(DepthEventName(DepthEvent::BeginEffects)) == "reshade_begin_effects");
    CHECK(std::string(DepthEventName(DepthEvent::FinishEffects)) == "reshade_finish_effects");
    CHECK(std::string(DepthEventName(DepthEvent::DestroyEffectRuntime)) == "destroy_effect_runtime");
    CHECK(std::string(DepthEventName(DepthEvent::ReloadedEffects)) == "reshade_reloaded_effects");
    CHECK(std::string(DepthEventName(DepthEvent::TechniqueState)) == "reshade_set_technique_state");
    CHECK(std::string(DepthEventName(DepthEvent::PresetPath)) == "reshade_set_current_preset_path");
}

void LateEvents()
{
    // Exactly the six depth callbacks. The last two only clear the manual transform cache; the boundary tracking subscribes the
    // same two ReShade events for its own callbacks, so the event kinds overlap but the callbacks do not. Not in the list: the
    // probe (present, reshade_present, init_effect_runtime) and the window mode (set_fullscreen_state).
    const auto& events = LateRegistrationPlanner::kEvents;
    static_assert(LateRegistrationPlanner::kEvents.size() == 6);
    bool seen[6] = {};
    for (DepthEvent e : events)
    {
        CHECK(static_cast<size_t>(e) < 6);
        seen[static_cast<size_t>(e)] = true;
        const std::string name = DepthEventName(e);
        CHECK(name == "reshade_begin_effects" || name == "reshade_finish_effects" || name == "destroy_effect_runtime" ||
              name == "reshade_reloaded_effects" || name == "reshade_set_technique_state" ||
              name == "reshade_set_current_preset_path");
    }
    CHECK(seen[0] && seen[1] && seen[2] && seen[3] && seen[4] && seen[5]);
}

void LatePlanner()
{
    using P = LateRegistrationPlanner;
    static_assert(P::kRetryIntervalMs == 1000);
    static_assert(P::kMaxAttempts == 60);
    constexpr auto kNoModule = RegisterStatus::NoModule;
    // the one case that retries: enabled, an attempt found no ReShade, a second has passed, the attempts are not used up
    CHECK(P::ShouldRetry(true, true, kNoModule, 0, 5000, 4000));
    CHECK(P::ShouldRetry(true, true, kNoModule, 0, 5001, 4000));
    CHECK(P::ShouldRetry(true, true, kNoModule, 0, 100000, 0));
    // not before a second is over
    CHECK(!P::ShouldRetry(true, true, kNoModule, 0, 4999, 4000));
    CHECK(!P::ShouldRetry(true, true, kNoModule, 0, 4000, 4000));
    CHECK(!P::ShouldRetry(true, true, kNoModule, 0, 3000, 4000)); // the clock went backwards
    // a bounded window: 59 late attempts so far still retries, 60 does not
    CHECK(P::ShouldRetry(true, true, kNoModule, 1, 5000, 4000));
    CHECK(P::ShouldRetry(true, true, kNoModule, 59, 5000, 4000));
    CHECK(!P::ShouldRetry(true, true, kNoModule, 60, 5000, 4000));
    CHECK(!P::ShouldRetry(true, true, kNoModule, 61, 5000, 4000));
    CHECK(!P::ShouldRetry(true, true, kNoModule, 100000, 5000, 4000));
    // option off
    CHECK(!P::ShouldRetry(false, true, kNoModule, 0, 100000, 0));
    // the load-time attempt has not run: it owns the first attempt
    CHECK(!P::ShouldRetry(true, false, kNoModule, 0, 100000, 0));
    // never register twice, and every other result is final
    const RegisterStatus finals[] = { RegisterStatus::Ok, RegisterStatus::NoSelf, RegisterStatus::ExportsMissing,
                                      RegisterStatus::Refused, RegisterStatus::Exception };
    for (RegisterStatus s : finals)
        CHECK(!P::ShouldRetry(true, true, s, 0, 100000, 0));
    CHECK(!P::ShouldRetry(false, true, RegisterStatus::Ok, 0, 100000, 0));
    CHECK(!P::ShouldRetry(true, false, RegisterStatus::Ok, 0, 100000, 0));
}
} // namespace

int main()
{
    Freshness();
    NeutralDefinition();
    LogRule();
    Names();
    LateEvents();
    LatePlanner();
    if (g_fail)
    {
        std::printf("reshade_depth_helpers: %d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("reshade_depth_helpers: all checks passed\n");
    return 0;
}
