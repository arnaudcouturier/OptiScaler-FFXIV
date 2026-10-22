#include "check.h"

#include "ext/ffxiv/jitter/FfxivJitterSourceCore.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace ext::ffxiv::jitter::source;

// The pure numbers of the source-level jitter fix: phase count, native strength, normalisation, projection, pairing, verification,
// the decision table and the log lines.

namespace
{
bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

float Halton(std::uint64_t index, unsigned base)
{
    float factor = 1, result = 0;
    for (; index; index /= base)
    {
        factor /= base;
        result += factor * float(index % base);
    }
    return result - 0.5f;
}

void TestPhaseCount()
{
    // The cases of the brief: 8 at native, 18 Quality, 24 Balanced, 32 Performance, 72 Ultra Performance (4K output).
    CHECK(PhaseCount(3840, 2160, 3840, 2160) == 8);
    CHECK(PhaseCount(2560, 1440, 3840, 2160) == 18);   // Quality 1.5x
    CHECK(PhaseCount(2227, 1253, 3840, 2160) == 24);   // Balanced 1.724x
    CHECK(PhaseCount(1920, 1080, 3840, 2160) == 32);   // Performance 2x
    CHECK(PhaseCount(1280, 720, 3840, 2160) == 72);    // Ultra Performance 3x
    // 1080p output, the same rule
    CHECK(PhaseCount(1920, 1080, 1920, 1080) == 8);
    CHECK(PhaseCount(960, 540, 1920, 1080) == 32);
    // exact integer rounding: an area ratio just above an integer rounds up, never down
    CHECK(PhaseCount(1000, 1000, 1001, 1000) == 9);    // 8.008 -> 9
    CHECK(PhaseCount(1001, 1000, 1001, 1000) == 8);
    // the upper clamp
    CHECK(PhaseCount(16, 16, 16384, 16384) == 4096);
    // invalid sizes: 0 = leave the game's phase count alone
    CHECK(PhaseCount(0, 1080, 3840, 2160) == 0);
    CHECK(PhaseCount(1920, 0, 3840, 2160) == 0);
    CHECK(PhaseCount(1920, 1080, 0, 2160) == 0);
    CHECK(PhaseCount(1920, 1080, 3840, 0) == 0);
    CHECK(PhaseCount(3841, 2160, 3840, 2160) == 0);    // render larger than the output
    CHECK(PhaseCount(3840, 2161, 3840, 2160) == 0);
    CHECK(PhaseCount(1920, 1080, 16385, 2160) == 0);   // beyond the sane extent
    CHECK(PhaseCount(16385, 1080, 16385, 2160) == 0);
}

void TestNativeStrength()
{
    // The game's expression: ratio = ext0/ext2 * scale; strength = (amount - 0.001) * ratio^2 + 0.001.
    // amount 1.0, scale 1.0, ext0 == ext2 -> 1.0 (the camera already draws the full lattice)
    auto s = NativeStrength(1.0f, 1.0f, 100, 100);
    CHECK(s && std::fabs(*s - 1.0f) < 1e-6f);
    // measured in game: strength 0.6. ratio = sqrt(0.6) with amount 1: ratio^2 = 0.6 -> (0.999 * 0.6) + 0.001 = 0.6004
    const float r = std::sqrt(0.6f);
    s = NativeStrength(1.0f, r, 1000, 1000);
    CHECK(s && std::fabs(*s - 0.6f) < 2e-3f);
    // order of operations as the executable does it: (amount - c) * (ratio * ratio) + c in float
    {
        const float amount = 0.8f, scale = 0.9f;
        const float ratio = (3000.0f / 4000.0f) * scale;
        const float expect = (amount - 0.001f) * (ratio * ratio) + 0.001f;
        const auto got = NativeStrength(amount, scale, 3000, 4000);
        CHECK(got && *got == expect);
    }
    // range [0.125, 4]
    CHECK(NativeStrength(0.01f, 1.0f, 100, 100).has_value() == false);   // 0.00999 + .001 -> below 0.125
    CHECK(NativeStrength(1.0f, 1.0f, 100, 100).has_value());
    CHECK(NativeStrength(5.0f, 1.0f, 100, 100).has_value() == false);    // 5.0 > 4
    CHECK(NativeStrength(4.0f, 1.0f, 100, 100).has_value());
    CHECK(NativeStrength(0.125f, 1.0f, 100, 100).has_value());           // (0.124) + 0.001 = 0.125 at the edge
    // unusable inputs
    CHECK(NativeStrength(1.0f, 1.0f, 100, 0).has_value() == false);      // extent 0
    CHECK(NativeStrength(NAN, 1.0f, 100, 100).has_value() == false);
    CHECK(NativeStrength(1.0f, INFINITY, 100, 100).has_value() == false);
    CHECK(NativeStrength(1.0f, NAN, 100, 100).has_value() == false);
}

void TestNormalize()
{
    float x = 0.3f, y = -0.2f;
    CHECK(NormalizeHalton(x, y, 0.6f));
    CHECK(std::fabs(x - 0.5f) < 1e-6f && std::fabs(y + 0.2f / 0.6f) < 1e-6f);
    // the game then multiplies by the strength: the drawn value is the generator's own
    CHECK(std::fabs(x * 0.6f - 0.3f) < 1e-6f && std::fabs(y * 0.6f + 0.2f) < 1e-6f);
    // the full lattice: |h| up to 0.5 comes back as 0.5
    x = 0.5f;
    y = -0.5f;
    CHECK(NormalizeHalton(x, y, 0.6f));
    CHECK(std::fabs(x * 0.6f - 0.5f) < 1e-6f);
    // strength 1: nothing to divide, and the frame counts as normalised (the phase count is replaced for it; one predicate for both decisions,
    // changed deliberately from the first version, which reported false and left the fix "inactive")
    x = 0.25f;
    y = 0.25f;
    CHECK(NormalizeHalton(x, y, 1.0f));
    CHECK(x == 0.25f && y == 0.25f);
    CHECK(NormalizeHalton(x, y, 1.0f + 5e-7f) && x == 0.25f);
    CHECK(NormalizeHalton(x, y, 1.0f + 1e-5f) && x < 0.25f); // just outside the equality window: divided
    x = 0.7f; // an out-of-range output at strength 1 is still refused
    CHECK(!NormalizeHalton(x, y, 1.0f) && x == 0.7f);
    x = y = 0.25f;
    // not a Halton value, or an unusable strength: unchanged
    for (float bad : { 0.6f, 0.0f, 0.1f, 5.0f, NAN, INFINITY })
    {
        x = 0.3f;
        y = 0.3f;
        const bool changed = NormalizeHalton(x, y, bad);
        const bool expect = bad >= 0.125f && bad <= 4.0f && std::isfinite(bad);
        CHECK(changed == expect);
        if (!expect)
            CHECK(x == 0.3f && y == 0.3f);
    }
    x = 0.6f;
    y = 0.1f;
    CHECK(!NormalizeHalton(x, y, 0.6f) && x == 0.6f && y == 0.1f); // out of the Halton range
    x = NAN;
    y = 0.1f;
    CHECK(!NormalizeHalton(x, y, 0.6f));
    x = 0.2f;
    y = INFINITY;
    CHECK(!NormalizeHalton(x, y, 0.6f) && x == 0.2f);
}

void TestPlan()
{
    GeneratorInputs in;
    in.dlssMode = true;
    in.amount = 1.0f;
    in.scale = std::sqrt(0.6f);
    in.extent0 = in.extent2 = 1000;
    in.phases = kNativePhases;
    in.renderW = 1920;
    in.renderH = 1080;
    in.outW = 3840;
    in.outH = 2160;
    GeneratorPlan p = PlanGenerate(in);
    CHECK(p.why == PlanWhy::Normalize && p.phases == 32 && std::fabs(p.strength - 0.6f) < 2e-3f);
    // quality change: the phase count follows the new sizes
    in.renderW = 2560;
    in.renderH = 1440;
    p = PlanGenerate(in);
    CHECK(p.why == PlanWhy::Normalize && p.phases == 18);
    // the game's own phase count (not unbounded) is kept
    in.phases = 12;
    p = PlanGenerate(in);
    CHECK(p.why == PlanWhy::Normalize && p.phases == 12);
    // sizes not usable: the phase count stays the game's, the amplitude is still normalised
    in.phases = kNativePhases;
    in.outW = 0;
    p = PlanGenerate(in);
    CHECK(p.why == PlanWhy::Normalize && p.phases == kNativePhases);
    in.outW = 3840;
    // not the DLSS mode: left alone
    in.dlssMode = false;
    p = PlanGenerate(in);
    CHECK(p.why == PlanWhy::NotDlssMode && p.phases == kNativePhases);
    in.dlssMode = true;
    // an invalid strength: left alone, phases untouched
    in.extent2 = 0;
    p = PlanGenerate(in);
    CHECK(p.why == PlanWhy::StrengthInvalid && p.phases == kNativePhases);
}

// Observed sample of a game that draws strength * jitter: ndc = 2 * strength * jitter / size (y negated).
Observed Sample(float rawX, float rawY, float strength, std::uint32_t w, std::uint32_t h, std::uint64_t t)
{
    Observed o;
    o.rawX = rawX;
    o.rawY = rawY;
    o.ndcX = 2.0f * strength * rawX / static_cast<float>(w);
    o.ndcY = -(2.0f * strength * rawY) / static_cast<float>(h);
    o.width = w;
    o.height = h;
    o.timeMs = t;
    return o;
}

void TestProjection()
{
    const Observed o = Sample(0.5f, -0.3f, 0.6f, 1920, 1080, 100);
    const auto p = ProjectOffset(o);
    CHECK(p && std::fabs(p->x - 0.3f) < 1e-5f && std::fabs(p->y + 0.18f) < 1e-5f);
    // x = ndcX * width / 2, y = -ndcY * height / 2
    Observed q;
    q.rawX = 0.4f;
    q.rawY = 0.4f;
    q.ndcX = 0.001f;
    q.ndcY = -0.001f;
    q.width = 2000;
    q.height = 1000;
    const auto pq = ProjectOffset(q);
    CHECK(!pq); // x = 1.0, y = 0.5: the axes do not describe one strength (x*rawY != y*rawX)
    q.ndcY = -0.002f;
    const auto pr = ProjectOffset(q);
    CHECK(pr && std::fabs(pr->x - 1.0f) < 1e-6f && std::fabs(pr->y - 1.0f) < 1e-6f);
    // the jitter-disabled path leaves old NDC storage: raw 0/0 -> 0/0 whatever the NDC holds
    Observed z;
    z.ndcX = 0.01f;
    z.ndcY = 0.01f;
    z.width = 1920;
    z.height = 1080;
    const auto pz = ProjectOffset(z);
    CHECK(pz && pz->x == 0.0f && pz->y == 0.0f);
    // refusals
    Observed bad = o;
    bad.width = 0;
    CHECK(!ProjectOffset(bad));
    bad = o;
    bad.height = 20000;
    CHECK(!ProjectOffset(bad));
    bad = o;
    bad.rawX = NAN;
    CHECK(!ProjectOffset(bad));
    bad = o;
    bad.ndcY = INFINITY;
    CHECK(!ProjectOffset(bad));
    bad = o;
    bad.rawX = 5.0f; // above 4
    CHECK(!ProjectOffset(bad));
    bad = o;
    bad.ndcX = -o.ndcX; // wrong sign against the raw value
    bad.ndcY = -o.ndcY;
    CHECK(!ProjectOffset(bad));
    bad = o;
    bad.ndcX = 0.01f; // 9.6 px: above 2
    CHECK(!ProjectOffset(bad));
    bad = o;
    bad.rawY = 0.0f; // an axis with no raw jitter but an NDC translation
    CHECK(!ProjectOffset(bad));
}

void TestRingPairing()
{
    Ring<Observed, kRingSize> ring;
    const float s = 0.6f;
    // 40 frames: the ring keeps the newest 32
    for (unsigned i = 0; i < 40; ++i)
        ring.Push(Sample(Halton(i + 1, 2) / s, Halton(i + 1, 3) / s, s, 1920, 1080, 1000 + i * 8));
    CHECK(ring.Size() == kRingSize);
    const std::uint64_t now = 1000 + 39 * 8 + 4;
    // the newest frame
    auto m = PairSample(ring, Halton(40, 2) / s, Halton(40, 3) / s, 1920, 1080, now);
    CHECK(m && std::fabs(m->pixels.x - Halton(40, 2)) < 1e-5f && std::fabs(m->pixels.y - Halton(40, 3)) < 1e-5f);
    // a frame 31 frames back is still in the ring
    m = PairSample(ring, Halton(9, 2) / s, Halton(9, 3) / s, 1920, 1080, now);
    CHECK(m && std::fabs(m->pixels.x - Halton(9, 2)) < 1e-5f);
    // frame 1..8 left the ring
    CHECK(!PairSample(ring, Halton(8, 2) / s, Halton(8, 3) / s, 1920, 1080, now));
    // the raw pair must match exactly (float noise only)
    CHECK(!PairSample(ring, Halton(40, 2) / s + 1e-4f, Halton(40, 3) / s, 1920, 1080, now));
    CHECK(PairSample(ring, Halton(40, 2) / s + 5e-8f, Halton(40, 3) / s, 1920, 1080, now));
    // sizes: within one pixel, otherwise no match
    CHECK(PairSample(ring, Halton(40, 2) / s, Halton(40, 3) / s, 1921, 1079, now));
    CHECK(!PairSample(ring, Halton(40, 2) / s, Halton(40, 3) / s, 1923, 1080, now));
    CHECK(!PairSample(ring, Halton(40, 2) / s, Halton(40, 3) / s, 0, 0, now));
    // age: younger than a second
    CHECK(PairSample(ring, Halton(40, 2) / s, Halton(40, 3) / s, 1920, 1080, 1000 + 39 * 8 + 999));
    CHECK(!PairSample(ring, Halton(40, 2) / s, Halton(40, 3) / s, 1920, 1080, 1000 + 39 * 8 + 1000));
    // a clock that went backwards counts as no time passed
    CHECK(PairSample(ring, Halton(40, 2) / s, Halton(40, 3) / s, 1920, 1080, 5));
    // not finite NGX values
    CHECK(!PairSample(ring, NAN, 0.1f, 1920, 1080, now));
    // the same raw pair twice (a repeating pattern): the newest wins
    Ring<Observed, kRingSize> rep;
    rep.Push(Sample(0.2f, 0.2f, 0.6f, 1920, 1080, 10));
    rep.Push(Sample(0.2f, 0.2f, 0.5f, 1920, 1080, 20)); // another strength, the same raw pair
    auto newest = PairSample(rep, 0.2f, 0.2f, 1920, 1080, 30);
    CHECK(newest && std::fabs(newest->pixels.x - 0.1f) < 1e-6f);
    // a sample whose offset cannot be computed is skipped, an older good one is used
    Ring<Observed, kRingSize> skip;
    skip.Push(Sample(0.2f, 0.2f, 0.6f, 1920, 1080, 10));
    Observed torn = Sample(0.2f, 0.2f, 0.6f, 1920, 1080, 20);
    torn.ndcX = NAN;
    skip.Push(torn);
    auto older = PairSample(skip, 0.2f, 0.2f, 1920, 1080, 30);
    CHECK(older && std::fabs(older->pixels.x - 0.12f) < 1e-6f);
    // an empty ring, and a sample with time 0 (never recorded)
    Ring<Observed, kRingSize> empty;
    CHECK(!PairSample(empty, 0.1f, 0.1f, 1920, 1080, 10));
    Ring<Observed, kRingSize> zero;
    zero.Push(Sample(0.2f, 0.2f, 0.6f, 1920, 1080, 0));
    CHECK(!PairSample(zero, 0.2f, 0.2f, 1920, 1080, 10));
}

void TestWatchdog()
{
    Watchdog w;
    CHECK(w.Verdict() == Fallback::None);
    for (unsigned i = 0; i < 10; ++i)
        w.Checked(0.0001f);
    CHECK(w.Verdict() == Fallback::None && w.totals().checked == 10 && w.totals().outside == 0);
    // outside the tolerance: only a RUN falls back
    for (unsigned i = 0; i < kOutsideToleranceLimit - 1; ++i)
        w.Checked(0.2f);
    CHECK(w.Verdict() == Fallback::None);
    w.Checked(0.0001f); // a good frame ends the run
    for (unsigned i = 0; i < kOutsideToleranceLimit - 1; ++i)
        w.Checked(0.2f);
    CHECK(w.Verdict() == Fallback::None);
    w.Checked(0.2f);
    CHECK(w.Verdict() == Fallback::OutsideTolerance);
    CHECK(std::fabs(w.totals().maxDeviation - 0.2f) < 1e-6f);
    // the tolerance edge
    Watchdog edge;
    for (unsigned i = 0; i < kOutsideToleranceLimit + 5; ++i)
        edge.Checked(kVerifyTolerancePx);
    CHECK(edge.Verdict() == Fallback::None && edge.totals().outside == 0);
    // unpaired evaluates: a run, ended by any paired evaluate
    Watchdog u;
    for (unsigned i = 0; i < kUnpairedLimit - 1; ++i)
        u.Unpaired();
    CHECK(u.Verdict() == Fallback::None);
    u.Unverified();
    for (unsigned i = 0; i < kUnpairedLimit - 1; ++i)
        u.Unpaired();
    CHECK(u.Verdict() == Fallback::None);
    u.Unpaired();
    CHECK(u.Verdict() == Fallback::Unpaired && u.totals().unpaired == 2 * kUnpairedLimit - 1);
    u.Reset();
    CHECK(u.Verdict() == Fallback::None && u.totals().unpaired == 0);
    // the limit is short: a miss is no longer harmless (it used to tell the divided raw value for up to 600 evaluates)
    CHECK(kUnpairedLimit >= 60 && kUnpairedLimit <= 120);
    // told from the generator's record: counted, but still a miss for the run (a pairing that is broken for good must fall back)
    Watchdog rec;
    for (unsigned i = 0; i < kUnpairedLimit; ++i)
    {
        rec.Unpaired();
        rec.ToldFromRecord();
    }
    CHECK(rec.Verdict() == Fallback::Unpaired && rec.totals().toldFromRecord == kUnpairedLimit && rec.totals().unpaired == kUnpairedLimit);
    // a value that cannot be told at all falls back at once, whatever the run
    Watchdog cannot;
    CHECK(cannot.Verdict() == Fallback::None);
    cannot.Untellable();
    CHECK(cannot.Verdict() == Fallback::Untellable && cannot.totals().untellable == 1);
    cannot.Reset();
    CHECK(cannot.Verdict() == Fallback::None);
    // Deviation is the larger axis error
    Generated g;
    g.expectedX = 0.3f;
    g.expectedY = -0.2f;
    CHECK(std::fabs(Deviation({ 0.31f, -0.2f }, g) - 0.01f) < 1e-6f);
    CHECK(std::fabs(Deviation({ 0.3f, -0.25f }, g) - 0.05f) < 1e-6f);
}

void TestDecision()
{
    DecisionInput in;
    in.keyOn = in.buildMatched = in.guardsOk = in.hooksInstalled = in.generatorRecent = true;
    Decision d = Decide(in);
    CHECK(d.active && d.reason == Reason::None);
    // every single missing condition makes it inactive with its own reason, in the order of the table
    { auto i = in; i.keyOn = false; d = Decide(i); CHECK(!d.active && d.reason == Reason::KeyOff); }
    { auto i = in; i.buildMatched = false; d = Decide(i); CHECK(!d.active && d.reason == Reason::ExecutableNotMatched); }
    { auto i = in; i.guardsOk = false; d = Decide(i); CHECK(!d.active && d.reason == Reason::GuardFailed); }
    { auto i = in; i.hooksInstalled = false; d = Decide(i); CHECK(!d.active && d.reason == Reason::HooksNotInstalled); }
    { auto i = in; i.fellBack = true; d = Decide(i); CHECK(!d.active && d.reason == Reason::FellBack); }
    { auto i = in; i.generatorRecent = false; d = Decide(i); CHECK(!d.active && d.reason == Reason::GeneratorIdle); }
    // the order when several fail
    { auto i = in; i.keyOn = false; i.buildMatched = false; d = Decide(i); CHECK(d.reason == Reason::KeyOff); }
    { auto i = in; i.buildMatched = false; i.guardsOk = false; d = Decide(i); CHECK(d.reason == Reason::ExecutableNotMatched); }
    { auto i = in; i.guardsOk = false; i.hooksInstalled = false; d = Decide(i); CHECK(d.reason == Reason::GuardFailed); }
    { auto i = in; i.hooksInstalled = false; i.fellBack = true; d = Decide(i); CHECK(d.reason == Reason::HooksNotInstalled); }
    // nothing set at all
    CHECK(!Decide(DecisionInput {}).active);
}

void TestSharedState()
{
    SharedState st;
    CHECK(!st.Enabled());
    st.Enable(true);
    CHECK(st.Enabled());
    st.Enable(false);
    CHECK(!st.Enabled());

    CHECK(!st.Status(100).recent);
    Generated g;
    g.outX = 0.5f;
    g.outY = -0.25f;
    g.expectedX = 0.3f;
    g.expectedY = -0.15f;
    g.strength = 0.6f;
    g.phases = 32;
    g.renderW = 1920;
    g.renderH = 1080;
    g.outW = 3840;
    g.outH = 2160;
    g.normalized = true;
    g.timeMs = 1000;
    st.RecordGenerated(g, PlanWhy::Normalize);
    auto status = st.Status(1500);
    CHECK(status.recent && status.phases == 32 && status.renderW == 1920 && status.outH == 2160 && std::fabs(status.strength - 0.6f) < 1e-6f);
    CHECK(!st.Status(1000 + kRecentMs).recent);          // the second is over
    CHECK(st.Status(900).recent);                          // a clock going backwards counts as no time passed
    auto found = st.FindGenerated(0.5f, -0.25f);
    CHECK(found && found->normalized && found->expectedX == 0.3f);
    CHECK(!st.FindGenerated(0.5f, -0.2f));
    // a call left alone is remembered as the reason but does not make the generator "recent"
    st.NoteLeftAlone(PlanWhy::StrengthInvalid);
    CHECK(st.Status(5000).lastWhy == PlanWhy::StrengthInvalid && !st.Status(5000).recent);

    Observed o = Sample(0.5f, -0.25f, 0.6f, 1920, 1080, 1100);
    st.RecordObserved(o);
    auto m = st.Pair(0.5f, -0.25f, 1920, 1080, 1200);
    CHECK(m && std::fabs(m->pixels.x - 0.3f) < 1e-5f);
    st.Clear();
    CHECK(!st.Pair(0.5f, -0.25f, 1920, 1080, 1200) && !st.FindGenerated(0.5f, -0.25f) && !st.Status(1200).recent);
}

void TestText()
{
    CHECK(FormatActive(0.6f, 32, 1920, 1080, 3840, 2160) == "Jitter source: active (strength 0.600, phases 32 for render 1920x1080 output 3840x2160)");
    CHECK(FormatInactive(Reason::ExecutableNotMatched) == "Jitter source: inactive (executable not matched)");
    CHECK(FormatInactive(Reason::KeyOff) == "Jitter source: inactive (key off)");
    CHECK(Has(FormatPhasesChanged(32, 18, 0.6f, 2560, 1440, 3840, 2160), "phases 32 -> 18"));
    CHECK(Has(FormatStandDown(), "jitter correction stands down"));
    Watchdog::Totals t;
    t.checked = 600;
    t.maxDeviation = 0.00012f;
    t.unverified = 3;
    t.unpaired = 2;
    const std::string v = FormatVerified(t);
    t.toldFromRecord = 1;
    const std::string v2 = FormatVerified(t);
    CHECK(Has(v2, "2 evaluates without a camera sample (1 told the generator's recorded value, 1 passed through)"));
    t.toldFromRecord = 0;
    CHECK(Has(v, "verified 600 frames") && Has(v, "max deviation 0.00012 px") && Has(v, "outside 0.005 px: 0") && Has(v, "3 frames without a generator record") &&
          Has(v, "2 evaluates without a camera sample"));
    const std::string f = FormatFellBack(Fallback::OutsideTolerance, t);
    CHECK(Has(f, "fell back to the old behaviour") && Has(f, "disabled together") && Has(f, "differs from the generator's Halton value"));
    CHECK(Has(FormatFellBack(Fallback::Unpaired, t), "no camera sample matched"));
    CHECK(Has(FormatFellBack(Fallback::Untellable, t), "cannot be told"));
    CHECK(Has(FormatSizeDisagreement(3840, 2160, 2560, 1440), "3840x2160") && Has(FormatSizeDisagreement(3840, 2160, 2560, 1440), "2560x1440"));
}

// The whole chain on numbers: generator -> normalise -> the game multiplies and projects -> the drawn offset is the full lattice.
void TestChain()
{
    const float strength = 0.6f;
    for (unsigned frame = 1; frame <= 64; ++frame)
    {
        const unsigned phases = 32;
        const unsigned index = (frame % phases) + 1;
        float x = Halton(index, 2), y = Halton(index, 3);
        const float hx = x, hy = y;
        CHECK(NormalizeHalton(x, y, strength));
        const Observed o = Sample(x, y, strength, 1920, 1080, frame * 8);
        const auto p = ProjectOffset(o);
        CHECK(p.has_value());
        if (!p)
            continue;
        CHECK(std::fabs(p->x - hx) < 1e-5f && std::fabs(p->y - hy) < 1e-5f);
        Generated g;
        g.expectedX = hx;
        g.expectedY = hy;
        CHECK(Deviation(*p, g) <= kVerifyTolerancePx);
    }
    // the cycle repeats: 32 distinct points at Performance, frame 33 equals frame 1
    std::vector<std::pair<float, float>> seen;
    for (unsigned frame = 0; frame < 32; ++frame)
        seen.emplace_back(Halton(frame + 1, 2), Halton(frame + 1, 3));
    for (size_t i = 0; i < seen.size(); ++i)
        for (size_t j = i + 1; j < seen.size(); ++j)
            CHECK(seen[i] != seen[j]);
    // wrong strength: dividing by the wrong number makes the drawn offset miss the generator's value by more than the tolerance
    {
        float x = Halton(5, 2), y = Halton(5, 3);
        const float hx = x, hy = y;
        NormalizeHalton(x, y, 0.5f); // we believe 0.5, the game uses 0.6
        const auto p = ProjectOffset(Sample(x, y, 0.6f, 1920, 1080, 10));
        Generated g;
        g.expectedX = hx;
        g.expectedY = hy;
        CHECK(p && Deviation(*p, g) > kVerifyTolerancePx);
    }
}
} // namespace

int main()
{
    TestPhaseCount();
    TestNativeStrength();
    TestNormalize();
    TestPlan();
    TestProjection();
    TestRingPairing();
    TestWatchdog();
    TestDecision();
    TestSharedState();
    TestText();
    TestChain();
    return TEST_RESULT("jitter_source_core");
}
