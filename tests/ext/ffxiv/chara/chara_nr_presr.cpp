// Preview NR before SR (the pre-SR placement, key [FFXIV] PreviewNRBeforeSR), the pure rules.
// What this proves: the placement rule is PostSr for every input with the key off (or latched off, or either path unusable) and PreSr only when all
// conditions hold; with the key off the evaluate runs once on the plain colour and the pre-SR entry is never called; a pre-SR run that gave no image
// (preparing, skipped, failed, unavailable, an image next to a non-Ran outcome) leaves the SR evaluate on the plain colour, never starts the post-SR
// NR for that render and withdraws the older NR result; a refused NR colour is evaluated again on the plain colour in the same render and latches
// NR before SR off exactly once, with the magenta picture; the cyan tint and "composed from NR" follow only an evaluate that used the NR image; the
// stream shapes, result kinds, input checks, copy plan, latch, tallies and timer modes of the three placements; a placement flip is a rebuild after
// the settle time.
// What it cannot prove (the game session does): that the driver accepts the shared image as Color, and the picture quality.
#include "check.h"

#include "ext/ffxiv/chara/CharaDlssNgx.h"
#include "ext/ffxiv/chara/CharaNrPlumbing.h"
#include "ext/ffxiv/chara/CharaNrRules.h"

#include <string>
#include <vector>

using namespace ext::ffxiv::chara::nr;
namespace ngx = ext::ffxiv::chara::dlss::ngx;

namespace
{
using Log = std::vector<std::string>;

constexpr Placement kPlacements[] = { Placement::BringUp, Placement::PostSr, Placement::PreSr };
constexpr RunOutcome kOutcomes[] = { RunOutcome::Failed, RunOutcome::Ran, RunOutcome::Preparing, RunOutcome::NotAvailable, RunOutcome::Skipped, RunOutcome::Paused };
constexpr RunOutcome kNotRan[] = { RunOutcome::Failed, RunOutcome::Preparing, RunOutcome::NotAvailable, RunOutcome::Skipped, RunOutcome::Paused };
constexpr long kOutcomeCount = sizeof(kOutcomes) / sizeof(kOutcomes[0]);
constexpr ResultKind kKinds[] = { ResultKind::None, ResultKind::BringUp, ResultKind::Production, ResultKind::PreSr };

PlumbingFacts FactsOf(int bits)
{
    PlumbingFacts f;
    f.srWanted = (bits & 1) != 0;
    f.nrWanted = (bits & 2) != 0;
    f.srLatched = (bits & 4) != 0;
    f.nrLatched = (bits & 8) != 0;
    f.nrAvailable = (bits & 16) != 0;
    f.srGateOpen = (bits & 32) != 0;
    f.nrHoldsResources = (bits & 64) != 0;
    f.preSrWanted = (bits & 128) != 0;
    f.preSrLatched = (bits & 256) != 0;
    return f;
}
constexpr int kFactsBits = 512;

PlumbingFacts BothOn()
{
    PlumbingFacts f;
    f.srWanted = f.nrWanted = f.srGateOpen = f.preSrWanted = true;
    return f;
}

// ---- The placement rule ------------------------------------------------------------------------------------------------------------------
void TestPlacementFor()
{
    long cases = 0;
    // PlacementFor has no "SR in charge" argument: it is asked from RunSlot only, which runs for a slot SR serves (a failed or too large slot never gets
    // there), so the answer depends on the facts alone
    for (int bits = 0; bits < kFactsBits; ++bits)
    {
        const PlumbingFacts f = FactsOf(bits);
        // typed independently of the helpers
        const bool srOk = f.srWanted && !f.srLatched && f.srGateOpen;
        const bool nrOk = f.nrWanted && !f.nrLatched && f.nrAvailable;
        const bool before = f.preSrWanted && !f.preSrLatched && srOk && nrOk;
        CHECK(PlacementFor(f) == (before ? Placement::PreSr : Placement::PostSr));
        if (!f.preSrWanted || f.preSrLatched)
            CHECK(PlacementFor(f) == Placement::PostSr); // key off or latched off: after SR, whatever else is true
        for (const bool recent : { false, true })
            for (const Placement p : kPlacements)
            {
                const bool run = p == Placement::PreSr && recent && srOk && nrOk;
                CHECK(RunPreSrNow(f, p, recent) == run);
            }
        ++cases;
    }
    CHECK(cases == kFactsBits);
    // each condition alone turns the placement back to after SR
    PlumbingFacts on = BothOn();
    CHECK(PlacementFor(on) == Placement::PreSr);
    PlumbingFacts f = on;
    f.preSrWanted = false;
    CHECK(PlacementFor(f) == Placement::PostSr);
    f = on;
    f.preSrLatched = true;
    CHECK(PlacementFor(f) == Placement::PostSr);
    f = on;
    f.nrWanted = false; // "no effect unless PreviewNR is on"
    CHECK(PlacementFor(f) == Placement::PostSr);
    f = on;
    f.srWanted = false; // "... and PreviewDLSS"
    CHECK(PlacementFor(f) == Placement::PostSr);
}

// ---- The SR evaluate with NR before it -------------------------------------------------------------------------------------------------
struct Fake
{
    Log log;
    ProductionAnswer answer { RunOutcome::Ran, true };
    bool nrEvaluate = true;    // the evaluate given the NR image succeeds
    bool plainEvaluate = true; // the evaluate given the plain colour succeeds
    int nrImage = 0, plainImage = 0;
    std::vector<const int*> colours; // what each evaluate was given
    std::vector<bool> resets;        // the history reset each evaluate was given

    ProductionAnswer RunPreSr()
    {
        log.push_back("presr");
        return answer;
    }
    bool Evaluate(SrColourSource source, bool reset)
    {
        log.push_back(source == SrColourSource::PreSr ? "eval-nr" : "eval-plain");
        colours.push_back(SrColourFor<const int*>(source, &nrImage, &plainImage));
        resets.push_back(reset);
        return source == SrColourSource::PreSr ? nrEvaluate : plainEvaluate;
    }
};

SrEvaluation Decide(const PlumbingFacts& f, bool recent, Fake& e, SrHistory history = {})
{
    return DecideSrEvaluate(f, recent, history, [&] { return e.RunPreSr(); }, [&](SrColourSource s, bool reset) { return e.Evaluate(s, reset); });
}

// Key off, latched off, or not eligible: the pre-SR entry is never called and the evaluate runs exactly once on the plain colour, as it did before.
void TestKeyOffIsUnchanged()
{
    long cases = 0;
    for (int bits = 0; bits < kFactsBits; ++bits)
    {
        const PlumbingFacts f = FactsOf(bits);
        for (const bool recent : { false, true })
            for (const bool plainOk : { false, true })
                for (const bool slotReset : { false, true })
                {
                    Fake e;
                    e.plainEvaluate = plainOk;
                    e.nrEvaluate = !plainOk; // would be different if it were ever used
                    // the slot's last evaluate was on the plain colour, as every evaluate is without NR before SR
                    const SrEvaluation r = Decide(f, recent, e, SrHistory { slotReset, SrColourSource::Plain });
                    ++cases;
                    if (r.placement != Placement::PreSr)
                    {
                        CHECK(e.log == (Log { "eval-plain" }));
                        CHECK(e.colours.size() == 1 && e.colours[0] == &e.plainImage);
                        CHECK(r.ok == plainOk && !r.preSrOffered && !r.preSrProduced && !r.usedPreSrColour && !r.latchPreSr);
                        // the evaluate gets exactly the slot's own reset: no extra reset reaches the default path
                        CHECK(e.resets.size() == 1 && e.resets[0] == slotReset && r.resetUsed == slotReset && r.colour == SrColourSource::Plain);
                    }
                    if (!f.preSrWanted)
                        CHECK(r.placement == Placement::PostSr);
                }
    }
    CHECK(cases == kFactsBits * 8);
}

void TestEvaluateOutcomes()
{
    const PlumbingFacts f = BothOn();

    // the NR image is produced and the evaluate takes it
    {
        Fake e;
        const SrEvaluation r = Decide(f, true, e);
        CHECK(e.log == (Log { "presr", "eval-nr" }));
        CHECK(e.colours.size() == 1 && e.colours[0] == &e.nrImage);
        CHECK(r.placement == Placement::PreSr && r.preSrOffered && r.preSrProduced && r.usedPreSrColour && r.ok && !r.latchPreSr);
    }
    // the evaluate refuses the NR image: run again on the plain colour in the same render, latch once, the NR image is not used
    {
        Fake e;
        e.nrEvaluate = false;
        const SrEvaluation r = Decide(f, true, e);
        CHECK(e.log == (Log { "presr", "eval-nr", "eval-plain" }));
        CHECK(e.colours.size() == 2 && e.colours[0] == &e.nrImage && e.colours[1] == &e.plainImage);
        CHECK(r.preSrProduced && !r.usedPreSrColour && r.ok && r.latchPreSr);
        // with the latch set the next render is after SR and never calls the entry
        PlumbingFacts latched = f;
        latched.preSrLatched = true;
        Fake next;
        const SrEvaluation n = Decide(latched, true, next);
        CHECK(n.placement == Placement::PostSr && next.log == (Log { "eval-plain" }) && !n.latchPreSr);
    }
    // both evaluates fail: the render fails as it always did (the caller fails the slot), and NR before SR is latched off all the same: the NR image
    // was given to the evaluate that failed first, so the failure may be the image's (a refused foreign image can leave the feature unusable for the
    // plain call of the same render); without the latch the slot would be released and fail again and again until the SR failure latch ends the path
    {
        Fake e;
        e.nrEvaluate = false;
        e.plainEvaluate = false;
        const SrEvaluation r = Decide(f, true, e);
        CHECK(e.log == (Log { "presr", "eval-nr", "eval-plain" }));
        CHECK(!r.ok && r.latchPreSr && !r.usedPreSrColour && r.preSrProduced);
    }
    // the pre-SR run gave no image, whatever the reason: the evaluate runs once on the plain colour, nothing is latched (also when that evaluate fails:
    // the NR image was never given to anything, so a plain-only failure is not the image's)
    for (const RunOutcome outcome : kNotRan)
        for (const bool hasImage : { false, true })
            for (const bool plainOk : { true, false })
            {
                Fake e;
                e.answer = { outcome, hasImage }; // an image next to a non-Ran outcome is not trusted
                e.nrEvaluate = false;
                e.plainEvaluate = plainOk;
                const SrEvaluation r = Decide(f, true, e);
                CHECK(e.log == (Log { "presr", "eval-plain" }));
                CHECK(e.colours[0] == &e.plainImage);
                CHECK(r.placement == Placement::PreSr && r.preSrOffered && !r.preSrProduced && !r.usedPreSrColour && r.ok == plainOk && !r.latchPreSr);
            }
    // Ran without an image: a defect of the module; plain colour this render, latched off (one line), not another try every render
    {
        Fake e;
        e.answer = { RunOutcome::Ran, false };
        const SrEvaluation r = Decide(f, true, e);
        CHECK(e.log == (Log { "presr", "eval-plain" }));
        CHECK(!r.preSrProduced && r.ok && r.latchPreSr);
        Fake both;
        both.answer = { RunOutcome::Ran, false };
        both.plainEvaluate = false;
        const SrEvaluation failed = Decide(f, true, both);
        CHECK(!failed.ok && failed.latchPreSr); // a defect of the module: latched whether or not the plain evaluate worked
    }
    // a window nobody shows: the entry is not called
    {
        Fake e;
        const SrEvaluation r = Decide(f, false, e);
        CHECK(e.log == (Log { "eval-plain" }) && r.placement == Placement::PreSr && !r.preSrOffered && !r.latchPreSr);
    }
    // NR unusable (latched, off, unavailable): after SR, the entry is not called
    for (int what = 0; what < 3; ++what)
    {
        PlumbingFacts n = f;
        n.nrLatched = what == 0;
        n.nrWanted = what != 1;
        n.nrAvailable = what != 2;
        Fake e;
        const SrEvaluation r = Decide(n, true, e);
        CHECK(r.placement == Placement::PostSr && e.log == (Log { "eval-plain" }));
    }
}

// A problem of the NR image never reaches the SR failure latch: a slot that fails every time the NR image is given to the evaluate would be released
// after 10 s, come back and fail again until four failures within a minute end the whole SR path. The latch set by the first render (RunSlot does
// `if (ev.latchPreSr) LatchPreSr()`, here fed back into the facts) makes the next render run after SR, so the NR image costs one failure at most.
void TestNrImageProblemCostsOneFailure()
{
    PlumbingFacts facts = BothOn();
    int slotFailures = 0, offered = 0;
    for (int render = 0; render < 8; ++render)
    {
        bool imageGiven = false;
        const SrEvaluation r = DecideSrEvaluate(
            facts, true, SrHistory {},
            [&]
            {
                ++offered;
                return ProductionAnswer { RunOutcome::Ran, true };
            },
            [&](SrColourSource source, bool)
            {
                if (source == SrColourSource::PreSr)
                {
                    imageGiven = true; // refused, and the feature stays unusable for the plain call of this same render
                    return false;
                }
                return !imageGiven;
            });
        if (!r.ok)
            ++slotFailures;
        if (r.latchPreSr)
            facts.preSrLatched = true;
    }
    CHECK(slotFailures == 1 && offered == 1);
    CHECK(facts.preSrLatched);
}

// The glue's evaluate, recorded through the real ApplyEvaluate (the CharaDlssNgx.h pattern): the same lambda body as RunSlot's, with a fake in place of
// the NGX parameter object (chara_dlss_glue_text pins the text of the glue's lambda). Every value an evaluate sets is kept apart and checked against
// the inputs typed here, not against another call of the same function.
struct NgxRecorder
{
    const void* colour = nullptr;
    const void* output = nullptr;
    const void* depth = nullptr;
    const void* motion = nullptr;
    float jitterX = -99.0f, jitterY = -99.0f;
    uint32_t subrectW = 0, subrectH = 0;
    int reset = -1;
    void SetUInt(const char* n, uint32_t v)
    {
        if (std::string(n) == ngx::kSubrectWidth)
            subrectW = v;
        else if (std::string(n) == ngx::kSubrectHeight)
            subrectH = v;
    }
    void SetInt(const char* n, int v)
    {
        if (std::string(n) == ngx::kReset)
            reset = v;
    }
    void SetFloat(const char* n, float v)
    {
        if (std::string(n) == ngx::kJitterX)
            jitterX = v;
        else if (std::string(n) == ngx::kJitterY)
            jitterY = v;
    }
    void SetResource(const char* n, const void* r)
    {
        if (std::string(n) == ngx::kColor)
            colour = r;
        else if (std::string(n) == ngx::kOutput)
            output = r;
        else if (std::string(n) == ngx::kDepth)
            depth = r;
        else if (std::string(n) == ngx::kMotionVectors)
            motion = r;
    }
};

void TestColourGivenToTheEvaluate()
{
    int nrImage = 0, plainImage = 0, outImage = 0, depthImage = 0, motionImage = 0;
    ngx::EvaluateInputs in;
    in.width = 576;
    in.height = 960;
    in.jitterX = 0.25f;
    in.jitterY = -0.5f;
    in.reset = false; // the slot's own rule asks for no reset: every reset below is the rules' own
    struct Case
    {
        bool keyOn;
        ProductionAnswer answer;
        bool nrEvaluate;
        SrColourSource previous;       // the colour source of the slot's last successful evaluate
        std::vector<const void*> want; // the Color of each evaluate, in order
        std::vector<int> wantReset;    // the Reset of each evaluate, in order
        bool latch;
    };
    const SrColourSource P = SrColourSource::PreSr, L = SrColourSource::Plain;
    const Case cases[] = {
        { true, { RunOutcome::Ran, true }, true, P, { &nrImage }, { 0 }, false },                 // NR ran, the evaluate took it, same source as before
        { true, { RunOutcome::Ran, true }, true, L, { &nrImage }, { 1 }, false },                 // ... after plain renders: the source changed
        { true, { RunOutcome::Ran, true }, false, L, { &nrImage, &plainImage }, { 1, 1 }, true }, // refused: again on the plain colour, latch
        { true, { RunOutcome::Ran, true }, false, P, { &nrImage, &plainImage }, { 0, 1 }, true }, // refused: the retry resets although the first call did not
        { true, { RunOutcome::Preparing, false }, true, P, { &plainImage }, { 1 }, false },       // no image yet, after NR renders: the source changed
        { true, { RunOutcome::Preparing, false }, true, L, { &plainImage }, { 0 }, false },
        { true, { RunOutcome::Failed, false }, true, L, { &plainImage }, { 0 }, false },
        { true, { RunOutcome::Skipped, true }, true, L, { &plainImage }, { 0 }, false },          // an image next to a non-Ran outcome is not used
        { false, { RunOutcome::Ran, true }, true, L, { &plainImage }, { 0 }, false },             // key off: never the NR image, no reset
        { false, { RunOutcome::Ran, true }, true, P, { &plainImage }, { 1 }, false },             // key switched off after NR renders: one reset
    };
    for (const Case& c : cases)
    {
        PlumbingFacts f = BothOn();
        f.preSrWanted = c.keyOn;
        std::vector<NgxRecorder> recorded;
        const SrEvaluation r = DecideSrEvaluate(
            f, true, SrHistory { in.reset, c.previous }, [&] { return c.answer; },
            [&](SrColourSource source, bool reset)
            {
                NgxRecorder rec;
                // RunSlot's lambda: Color is SrColourFor(source, the NR result, g.color), the reset is the one it is given, the rest is the slot's own
                ngx::ApplyEvaluate<NgxRecorder, const void*>(rec, ngx::WithReset(in, reset), SrColourFor<const void*>(source, &nrImage, &plainImage),
                                                              &outImage, &depthImage, &motionImage);
                recorded.push_back(rec);
                return source == SrColourSource::PreSr ? c.nrEvaluate : true;
            });
        CHECK(recorded.size() == c.want.size() && recorded.size() == c.wantReset.size());
        for (size_t i = 0; i < recorded.size() && i < c.want.size() && i < c.wantReset.size(); ++i)
        {
            const NgxRecorder& rec = recorded[i];
            CHECK(rec.colour == c.want[i]);
            CHECK(rec.reset == c.wantReset[i]);
            // the retry carries the same output, guides, jitter and sub-rectangle as the first call
            CHECK(rec.output == &outImage && rec.depth == &depthImage && rec.motion == &motionImage);
            CHECK(rec.jitterX == 0.25f && rec.jitterY == -0.5f && rec.subrectW == 576 && rec.subrectH == 960);
        }
        CHECK(r.ok && r.latchPreSr == c.latch);
        CHECK(!c.want.empty() && r.resetUsed == (c.wantReset.back() != 0));
        CHECK(r.colour == (c.want.back() == &nrImage ? SrColourSource::PreSr : SrColourSource::Plain) && r.usedPreSrColour == (r.colour == SrColourSource::PreSr));
    }
}

// The history reset of an evaluate: the slot's own rule, a change of the colour source since the slot's last successful evaluate (NR image <-> plain
// image: another format and another look, so the upscaler's history does not belong to the new picture), or the retry after a refused call (what the
// refused call left in the feature's history is not known). Nothing else resets.
void TestResetRule()
{
    CHECK(!ResetOnSourceChange(SrColourSource::Plain, SrColourSource::Plain, true));
    CHECK(!ResetOnSourceChange(SrColourSource::PreSr, SrColourSource::PreSr, true));
    CHECK(ResetOnSourceChange(SrColourSource::Plain, SrColourSource::PreSr, true));
    CHECK(ResetOnSourceChange(SrColourSource::PreSr, SrColourSource::Plain, true));
    CHECK(!ResetOnSourceChange(SrColourSource::Plain, SrColourSource::PreSr, false) && !ResetOnSourceChange(SrColourSource::PreSr, SrColourSource::Plain, false));
    for (const bool slotReset : { false, true })
        for (const SrColourSource previous : { SrColourSource::Plain, SrColourSource::PreSr })
            for (const SrColourSource now : { SrColourSource::Plain, SrColourSource::PreSr })
                for (const bool afterRefusal : { false, true })
                    for (const bool due : { false, true })
                        CHECK(EvaluateResetFor(slotReset, previous, now, afterRefusal, due) == (slotReset || (previous != now && due) || afterRefusal));

    // A slot over a series of renders (the glue stores `ev.colour` after a successful evaluate): the reset is asked exactly on a change of the source.
    struct Step
    {
        RunOutcome outcome;
        bool resetExpected;
    };
    const Step steps[] = {
        { RunOutcome::Preparing, false }, // plain, as before
        { RunOutcome::Preparing, false },
        { RunOutcome::Ran, true }, // first NR image: plain -> NR
        { RunOutcome::Ran, false },
        { RunOutcome::Ran, false },
        { RunOutcome::Skipped, true }, // NR -> plain
        { RunOutcome::Failed, false },
        { RunOutcome::Ran, true }, // plain -> NR
        { RunOutcome::Ran, false },
    };
    const PlumbingFacts f = BothOn();
    SrColourSource previous = SrColourSource::Plain; // a fresh slot
    int count = 0;
    for (const Step& step : steps)
    {
        Fake e;
        e.answer = { step.outcome, step.outcome == RunOutcome::Ran };
        const SrEvaluation r = Decide(f, true, e, SrHistory { false, previous });
        CHECK(r.ok && e.resets.size() == 1);
        CHECK(e.resets[0] == step.resetExpected);
        if (r.ok)
            previous = r.colour;
        ++count;
    }
    CHECK(count == 9);
    // a refusal ends in the plain source and resets the retry; two failing evaluates reset both (the slot fails, so no source is stored)
    {
        Fake e;
        e.nrEvaluate = false;
        const SrEvaluation r = Decide(f, true, e, SrHistory { false, SrColourSource::PreSr });
        CHECK(r.ok && r.colour == SrColourSource::Plain && r.latchPreSr && e.resets == (std::vector<bool> { false, true }));
        Fake both;
        both.nrEvaluate = false;
        both.plainEvaluate = false;
        const SrEvaluation failed = Decide(f, true, both, SrHistory { false, SrColourSource::Plain });
        CHECK(!failed.ok && both.resets == (std::vector<bool> { true, true }));
    }
}

// The debounce of the source-change reset: a render-by-render alternation of the colour source (the pre-SR run skipped on every other render under GPU
// load) must not reset the upscaler's history on every evaluate. A source-change reset is granted at most once per 500 ms for a slot; the slot's own
// reset and the retry after a refused call are never held back.
void TestSourceResetDebounce()
{
    CHECK(kSourceResetMinMs == 500);
    // the clock: a fresh slot is due; 499 ms after a note is not, exactly 500 is; a clock that went backwards is due
    SourceResetClock clock;
    CHECK(clock.Due(0) && clock.Due(123456));
    clock.Note(1000);
    CHECK(!clock.Due(1000) && !clock.Due(1001) && !clock.Due(1499));
    CHECK(clock.Due(1500) && clock.Due(1501) && clock.Due(99999));
    CHECK(clock.Due(999)); // went backwards
    clock.Note(1500);
    CHECK(!clock.Due(1999) && clock.Due(2000));
    CHECK(!SourceResetClock {}.seen && SourceResetClock {}.Due(0));

    // The glue as a series: the history gets `clock.Due(now)`, a successful evaluate that ran with a source-change reset notes the time, the colour is stored.
    struct Step
    {
        uint64_t now;
        RunOutcome outcome;
        bool resetExpected;
        bool sourceResetExpected;
    };
    const Step steps[] = {
        { 0, RunOutcome::Preparing, false, false },  // plain, a fresh slot
        { 1000, RunOutcome::Ran, true, true },       // plain -> NR: the first change is granted
        { 1016, RunOutcome::Skipped, false, false }, // NR -> plain 16 ms later: held back
        { 1032, RunOutcome::Ran, false, false },     // plain -> NR: held back
        { 1499, RunOutcome::Skipped, false, false }, // 499 ms after the grant: held back
        { 1500, RunOutcome::Ran, true, true },       // exactly 500 ms: granted (the previous render was plain)
        { 1501, RunOutcome::Skipped, false, false },
        { 2000, RunOutcome::Ran, true, true },       // 500 ms after the last grant
        { 2001, RunOutcome::Ran, false, false },     // no change
        { 2600, RunOutcome::Skipped, true, true },   // NR -> plain, 600 ms after the last grant
    };
    const PlumbingFacts f = BothOn();
    SrColourSource previous = SrColourSource::Plain;
    SourceResetClock slot;
    int granted = 0, resets = 0;
    for (const Step& step : steps)
    {
        Fake e;
        e.answer = { step.outcome, step.outcome == RunOutcome::Ran };
        const SrEvaluation r = Decide(f, true, e, SrHistory { false, previous, slot.Due(step.now) });
        CHECK(r.ok && e.resets.size() == 1);
        CHECK(e.resets[0] == step.resetExpected);
        CHECK(r.sourceReset == step.sourceResetExpected);
        resets += e.resets[0] ? 1 : 0;
        if (r.ok)
            previous = r.colour;
        if (r.ok && r.sourceReset)
        {
            slot.Note(step.now);
            ++granted;
        }
    }
    CHECK(granted == 4 && resets == 4); // 10 renders with 8 source changes among them: 4 resets, not 8
    // the same series without the debounce resets on every change (what this replaces)
    {
        SrColourSource prev = SrColourSource::Plain;
        int every = 0;
        for (const Step& step : steps)
        {
            Fake e;
            e.answer = { step.outcome, step.outcome == RunOutcome::Ran };
            const SrEvaluation r = Decide(f, true, e, SrHistory { false, prev, true });
            every += e.resets[0] ? 1 : 0;
            prev = r.colour;
        }
        CHECK(every == 8);
    }

    // never held back: the slot's own reset, and the retry after a refused call (neither is counted as a source-change reset)
    for (const SrColourSource previousSource : { SrColourSource::Plain, SrColourSource::PreSr })
    {
        Fake own;
        const SrEvaluation r = Decide(f, true, own, SrHistory { true, previousSource, false });
        CHECK(r.ok && own.resets == (std::vector<bool> { true }) && r.resetUsed && !r.sourceReset);
    }
    {
        Fake refused;
        refused.nrEvaluate = false;
        const SrEvaluation r = Decide(f, true, refused, SrHistory { false, SrColourSource::Plain, false });
        CHECK(r.ok && r.latchPreSr && refused.resets == (std::vector<bool> { false, true }) && !r.sourceReset); // the NR call: change held back; the retry resets
        Fake refusedAfterNr;
        refusedAfterNr.nrEvaluate = false;
        const SrEvaluation s = Decide(f, true, refusedAfterNr, SrHistory { false, SrColourSource::PreSr, false });
        CHECK(s.ok && refusedAfterNr.resets == (std::vector<bool> { false, true }) && !s.sourceReset);
        Fake bothFail;
        bothFail.nrEvaluate = false;
        bothFail.plainEvaluate = false;
        const SrEvaluation t = Decide(f, true, bothFail, SrHistory { false, SrColourSource::Plain, false });
        CHECK(!t.ok && bothFail.resets == (std::vector<bool> { false, true }));
    }
    // due: the plain retry after NR renders is both a change and a refusal, and the flag says so
    {
        Fake e;
        e.nrEvaluate = false;
        const SrEvaluation r = Decide(f, true, e, SrHistory { false, SrColourSource::PreSr, true });
        CHECK(r.ok && e.resets == (std::vector<bool> { false, true }) && r.sourceReset);
    }
}

// The slot's own reset is the NR-image evaluate's reset too (and the plain branch's under the pre-SR placement). A pause longer than the slot's window must
// reset the history whatever the last colour source was, so `history.reset` must reach the evaluate: passing a literal false there passed every earlier test.
void TestSlotResetReachesTheEvaluate()
{
    const PlumbingFacts f = BothOn();
    for (const SrColourSource previous : { SrColourSource::Plain, SrColourSource::PreSr })
        for (const bool due : { false, true })
        {
            // NR image given and taken: the slot's reset, no more
            Fake e;
            const SrEvaluation r = Decide(f, true, e, SrHistory { true, previous, due });
            CHECK(r.ok && r.usedPreSrColour && e.resets == (std::vector<bool> { true }) && r.resetUsed);
            // NR image refused: both calls carry the slot's reset
            Fake refused;
            refused.nrEvaluate = false;
            const SrEvaluation s = Decide(f, true, refused, SrHistory { true, previous, due });
            CHECK(s.ok && refused.resets == (std::vector<bool> { true, true }));
            // no image (the plain branch under the pre-SR placement): the slot's reset
            Fake preparing;
            preparing.answer = { RunOutcome::Preparing, false };
            const SrEvaluation p = Decide(f, true, preparing, SrHistory { true, previous, due });
            CHECK(p.ok && p.placement == Placement::PreSr && !p.usedPreSrColour && preparing.resets == (std::vector<bool> { true }));
        }
    // and without it, with the source unchanged, no reset at all
    Fake none;
    const SrEvaluation n = Decide(f, true, none, SrHistory { false, SrColourSource::PreSr, true });
    CHECK(n.ok && none.resets == (std::vector<bool> { false }) && !n.resetUsed);
}

// ---- The slot step with the pre-SR placement -------------------------------------------------------------------------------------------
struct SlotFake
{
    Log log;
    ProductionAnswer answer { RunOutcome::Ran, true };
    bool alphaOk = true;
    AlphaInput inputSeen = AlphaInput::SrOutput;
    TintKind tintSeen = TintKind::None;
    ProductionAnswer Production()
    {
        log.push_back("production");
        return answer;
    }
    bool Alpha(AlphaInput input, TintKind tint)
    {
        log.push_back("alpha");
        inputSeen = input;
        tintSeen = tint;
        return alphaOk;
    }
};

SlotRender Slot(const PlumbingFacts& f, bool recent, bool tintOn, Placement placement, bool preSrUsed, SlotFake& e)
{
    return DecideSlotRender(f, recent, tintOn, placement, preSrUsed, [&] { return e.Production(); }, [&](AlphaInput i, TintKind t) { return e.Alpha(i, t); });
}

void TestSlotRenderPreSr()
{
    PlumbingFacts f = BothOn();
    f.nrHoldsResources = true;
    for (const bool tintOn : { false, true })
        for (const bool recent : { false, true })
            for (const RunOutcome outcome : kOutcomes)
                for (const bool hasView : { false, true })
                    for (const bool alphaOk : { false, true })
                    {
                        // the SR evaluate was given the NR image: cyan, composed from NR, the production entry is never offered
                        {
                            SlotFake e;
                            e.answer = { outcome, hasView };
                            e.alphaOk = alphaOk;
                            const SlotRender s = Slot(f, recent, tintOn, Placement::PreSr, true, e);
                            CHECK(e.log == (Log { "alpha" }));
                            CHECK(!s.offered && !s.produced && s.preSrColour && s.alphaInput == AlphaInput::SrOutput && e.inputSeen == AlphaInput::SrOutput);
                            CHECK(s.tint == (tintOn ? TintKind::Nr : TintKind::None) && e.tintSeen == s.tint);
                            CHECK(s.alphaOk == alphaOk && s.composedFromNr == alphaOk);
                        }
                        // the placement is PreSr but the evaluate was not given the NR image (no image, refused): plain SR picture, magenta, and still
                        // no production entry for this render
                        {
                            SlotFake e;
                            e.answer = { outcome, hasView };
                            e.alphaOk = alphaOk;
                            const SlotRender s = Slot(f, recent, tintOn, Placement::PreSr, false, e);
                            CHECK(e.log == (Log { "alpha" }));
                            CHECK(!s.offered && !s.produced && !s.preSrColour && !s.composedFromNr);
                            CHECK(s.tint == (tintOn ? TintKind::Sr : TintKind::None) && e.tintSeen == s.tint);
                        }
                        // "the pre-SR image was used" means nothing after SR: the post-SR rules apply unchanged
                        {
                            SlotFake with, without;
                            with.answer = without.answer = { outcome, hasView };
                            with.alphaOk = without.alphaOk = alphaOk;
                            const SlotRender a = Slot(f, recent, tintOn, Placement::PostSr, true, with);
                            const SlotRender b = Slot(f, recent, tintOn, Placement::PostSr, false, without);
                            CHECK(!a.preSrColour && a.offered == b.offered && a.produced == b.produced && a.tint == b.tint &&
                                  a.composedFromNr == b.composedFromNr && with.log == without.log);
                        }
                    }

    // the finisher after a pre-SR render: an NR image used by the evaluate is an NR result for the render (not withdrawn), a render without one is
    // withdrawn, and the bring-up never runs beside it (the SR path is in charge)
    {
        SlotFake e;
        const SlotRender used = Slot(f, true, false, Placement::PreSr, true, e);
        const FinisherRender fin = DecideFinisherRender(f, true, true, true, used.composedFromNr, [] { return true; });
        CHECK(fin.nrProducedForRender && !fin.invalidate && !fin.runBringUp);
        const SlotRender none = Slot(f, true, false, Placement::PreSr, false, e);
        const FinisherRender fin2 = DecideFinisherRender(f, true, true, true, none.composedFromNr, [] { return true; });
        CHECK(!fin2.nrProducedForRender && fin2.invalidate && !fin2.runBringUp);
    }
}

// The production statistics (runs and UI draws given a composed image) count the pictures made from the post-SR NR output, never those whose SR
// evaluate was given the pre-SR image: the two counters used to disagree for a pre-SR picture.
void TestPostSrNrStatistic()
{
    CHECK(MadeFromPostSrNr(true, false));
    CHECK(!MadeFromPostSrNr(true, true));
    CHECK(!MadeFromPostSrNr(false, false));
    CHECK(!MadeFromPostSrNr(false, true));
    PlumbingFacts f = BothOn();
    f.nrHoldsResources = true;
    long cases = 0;
    for (const Placement placement : { Placement::PostSr, Placement::PreSr })
        for (const bool preSrUsed : { false, true })
            for (const bool recent : { false, true })
                for (const RunOutcome outcome : kOutcomes)
                    for (const bool hasView : { false, true })
                        for (const bool alphaOk : { false, true })
                        {
                            SlotFake e;
                            e.answer = { outcome, hasView };
                            e.alphaOk = alphaOk;
                            const SlotRender s = Slot(f, recent, false, placement, preSrUsed, e);
                            // typed from the rules: only a production-entry image that went through the alpha pass counts
                            CHECK(MadeFromPostSrNr(s.composedFromNr, s.preSrColour) == (s.produced && s.alphaOk));
                            if (placement == Placement::PreSr)
                                CHECK(!MadeFromPostSrNr(s.composedFromNr, s.preSrColour));
                            ++cases;
                        }
    CHECK(cases == 2 * 2 * 2 * kOutcomeCount * 2 * 2);
}

void TestTintFor()
{
    for (const AlphaInput input : { AlphaInput::SrOutput, AlphaInput::NrOutput })
    {
        CHECK(AlphaTintFor(false, input) == TintKind::None && AlphaTintFor(false, input, true) == TintKind::None);
        CHECK(AlphaTintFor(true, input) == (input == AlphaInput::NrOutput ? TintKind::Nr : TintKind::Sr)); // two arguments: as before
        CHECK(AlphaTintFor(true, input, false) == AlphaTintFor(true, input));
        CHECK(AlphaTintFor(true, input, true) == TintKind::Nr); // the SR evaluate itself was given the NR image
    }
}

// ---- Stream shapes, kinds, fits ---------------------------------------------------------------------------------------------------------
constexpr uint32_t kW = 576, kH = 960;

StreamShape ShapeOf(Placement p, bool lateWanted = false)
{
    const uint32_t w = p == Placement::PostSr ? kW * 2 : kW, h = p == Placement::PostSr ? kH * 2 : kH;
    return ShapeFor(p, w, h, 2, 1, lateWanted, kW, kH);
}

void TestShapes()
{
    for (const Placement p : kPlacements)
    {
        const StreamShape s = ShapeOf(p);
        CHECK(PlacementOf(s) == p);
        CHECK(PlacementFrom(s.production, s.preSr) == p);
        CHECK(s.production == (p == Placement::PostSr) && s.preSr == (p == Placement::PreSr));
        CHECK((s.guideWidth != 0) == CarriesGuides(p) && (s.guideHeight != 0) == CarriesGuides(p));
        CHECK(std::string(PlacementName(p)) == (p == Placement::BringUp ? "bring-up" : p == Placement::PostSr ? "production" : "pre-SR"));
        // PreviewNRLate belongs to the bring-up streams only
        const StreamShape late = ShapeOf(p, true);
        CHECK(late.late == (p == Placement::BringUp));
        CHECK(LateVariantInUse(late) == (p == Placement::BringUp));
        for (const Placement q : kPlacements)
            CHECK((ShapeOf(p) == ShapeOf(q)) == (p == q));
    }
    // a pre-SR shape whose late flag was set by hand is still not late
    StreamShape forced = ShapeOf(Placement::PreSr);
    forced.late = true;
    CHECK(!LateVariantInUse(forced));
    // the pre-SR shape: render size colour, the guides the same size
    const StreamShape pre = ShapeOf(Placement::PreSr);
    CHECK(pre.width == kW && pre.height == kH && pre.guideWidth == kW && pre.guideHeight == kH && !pre.late);
    // the bring-up shape is what the module built before this existed
    const StreamShape bring = ShapeOf(Placement::BringUp);
    StreamShape expect;
    expect.width = kW;
    expect.height = kH;
    expect.preset = 2;
    expect.style = 1;
    CHECK(bring == expect);
    StreamShape late = expect;
    late.late = true;
    CHECK(ShapeOf(Placement::BringUp, true) == late);
    // and the production shape too
    StreamShape prod = expect;
    prod.width = kW * 2;
    prod.height = kH * 2;
    prod.production = true;
    prod.guideWidth = kW;
    prod.guideHeight = kH;
    CHECK(ShapeOf(Placement::PostSr, true) == prod);
}

void TestResultKinds()
{
    // exhaustive: every kind answers both predicates
    for (const ResultKind k : kKinds)
    {
        CHECK(ServesBringUpConsumer(k) == (k == ResultKind::BringUp));
        CHECK(ResultTextureServes(k) == (k == ResultKind::BringUp || k == ResultKind::Production));
    }
    CHECK(static_cast<int>(ResultKind::PreSr) == 3);
    for (const Placement p : kPlacements)
    {
        StreamState st;
        st.built = ShapeOf(p);
        st.hasBuilt = true;
        st.everRan = true;
        st.attempts = st.ranAttempt = 4;
        st.lastRun = 1000;
        st.runs = 4;
        const ResultKind want = p == Placement::BringUp ? ResultKind::BringUp : p == Placement::PostSr ? ResultKind::Production : ResultKind::PreSr;
        CHECK(ResultKindOf(st) == want);
        // a bring-up consumer is given a result only from a bring-up stream
        CHECK((ResultForDecision(st, false, 1100) >= 0) == (p == Placement::BringUp));
        st.NoteRenderWithoutResult();
        CHECK(ResultKindOf(st) == ResultKind::None);
    }
}

void TestInputsFit()
{
    const ImageInfo colour { kW, kH, kFormatRgba8Unorm }, depth { kW, kH, kFormatR32Float }, motion { kW, kH, kFormatRg16Float };
    const StreamShape pre = ShapeOf(Placement::PreSr);
    CHECK(PreSrInputsFit(pre, colour, depth, motion) && CopiedInputsFit(pre, colour, depth, motion));
    CHECK(!PreSrInputsFit(pre, { kW / 2, kH, kFormatRgba8Unorm }, depth, motion));
    CHECK(!PreSrInputsFit(pre, { kW, kH / 2, kFormatRgba8Unorm }, depth, motion));
    CHECK(!PreSrInputsFit(pre, { kW, kH, kFormatRgba16Float }, depth, motion)); // the SR output is not the input of this placement
    CHECK(!PreSrInputsFit(pre, { kW, kH, 87 /* B8G8R8A8 */ }, depth, motion));
    CHECK(!PreSrInputsFit(pre, colour, { kW * 2, kH, kFormatR32Float }, motion));
    CHECK(!PreSrInputsFit(pre, colour, { kW, kH, 40 /* D32 */ }, motion));
    CHECK(!PreSrInputsFit(pre, colour, depth, { kW, kH * 2, kFormatRg16Float }));
    CHECK(!PreSrInputsFit(pre, colour, depth, { kW, kH, kFormatR32Float }));
    // guides that are not the colour's size do not fit this placement
    StreamShape odd = pre;
    odd.guideWidth = kW / 2;
    CHECK(!PreSrInputsFit(odd, colour, { kW / 2, kH, kFormatR32Float }, { kW / 2, kH, kFormatRg16Float }));
    // a shape that is not pre-SR, an empty one, and one that claims both modes never fit
    CHECK(!PreSrInputsFit(ShapeOf(Placement::BringUp), colour, depth, motion));
    CHECK(!PreSrInputsFit(StreamShape {}, colour, depth, motion));
    StreamShape both = pre;
    both.production = true;
    CHECK(!PreSrInputsFit(both, colour, depth, motion));
    // the production check refuses a pre-SR shape (and a both-modes shape), and keeps accepting its own
    const ImageInfo outColour { kW * 2, kH * 2, kFormatRgba16Float };
    const StreamShape prod = ShapeOf(Placement::PostSr);
    CHECK(ProductionInputsFit(prod, outColour, depth, motion) && CopiedInputsFit(prod, outColour, depth, motion));
    CHECK(!ProductionInputsFit(pre, outColour, depth, motion));
    StreamShape prodAndPre = prod;
    prodAndPre.preSr = true;
    CHECK(!ProductionInputsFit(prodAndPre, outColour, depth, motion) && !PreSrInputsFit(prodAndPre, colour, depth, motion));
    // the dispatcher by placement: each placement accepts only its own inputs; the bring-up copies none
    CHECK(!CopiedInputsFit(pre, outColour, depth, motion) && !CopiedInputsFit(prod, colour, depth, motion));
    CHECK(!CopiedInputsFit(ShapeOf(Placement::BringUp), colour, depth, motion));
}

void TestFormatsAndCopies()
{
    constexpr uint32_t kNative = 87; // B8G8R8A8_UNORM, the preview image
    CHECK(ColourFormatFor(Placement::BringUp, kNative) == kNative);
    CHECK(ColourFormatFor(Placement::PostSr, kNative) == kFormatRgba16Float);
    CHECK(ColourFormatFor(Placement::PreSr, kNative) == kFormatRgba8Unorm && kFormatRgba8Unorm == 28);
    for (const Placement p : kPlacements)
    {
        const CopyPlan plan = CopyPlanFor(p);
        CHECK(plan.count == (CarriesGuides(p) ? 3u : 1u));
        CHECK(plan.kinds[0] == CopyKind::Colour); // the colour first, then the guides, all before the ready signal
        if (CarriesGuides(p))
            CHECK(plan.kinds[1] == CopyKind::Depth && plan.kinds[2] == CopyKind::Motion);
    }
}

// A placement flip rebuilds the stream, after it has held still for the settle time; a flip back in time keeps it.
void TestPlacementFlip()
{
    for (const Placement from : kPlacements)
        for (const Placement to : kPlacements)
        {
            StreamState st;
            st.built = ShapeOf(from);
            st.hasBuilt = true;
            const StreamShape wanted = ShapeOf(to);
            if (from == to)
            {
                CHECK(DecideRebuild(st, wanted, 1000) == RebuildAction::Keep);
                continue;
            }
            CHECK(DecideRebuild(st, wanted, 1000) == RebuildAction::Settle);
            CHECK(DecideRebuild(st, wanted, 1000 + kRebuildSettleMs - 1) == RebuildAction::Settle);
            CHECK(DecideRebuild(st, wanted, 1000 + kRebuildSettleMs) == RebuildAction::Rebuild);
            // a flip that goes back before the settle time is dropped
            StreamState back;
            back.built = ShapeOf(from);
            back.hasBuilt = true;
            CHECK(DecideRebuild(back, wanted, 1000) == RebuildAction::Settle);
            CHECK(DecideRebuild(back, ShapeOf(from), 1100) == RebuildAction::Keep && !back.hasPending);
        }
}

void TestLatchesAndCounters()
{
    // the pre-SR latch is its own: it does not touch the SR or the NR latch, and they do not touch it
    Latches latches;
    CHECK(!latches.PreSrLatched());
    CHECK(latches.LatchPreSr() && !latches.LatchPreSr() && latches.PreSrLatched()); // true for the call that set it only
    CHECK(!latches.SrLatched() && !latches.NrLatched());
    Latches other;
    CHECK(other.LatchSr() && other.LatchNr() && !other.PreSrLatched());
    Latches onlyNr;
    onlyNr.LatchNr();
    CHECK(!onlyNr.PreSrLatched() && !onlyNr.SrLatched());

    // the tally counts a call that produced no result once, in `all` and in its own entry's counter
    FallbackTally tally;
    for (const Placement p : kPlacements)
    {
        const RunOutcome o = CountedCall(tally, p, [] { return RunOutcome::Skipped; }, [](RunOutcome r) { return r == RunOutcome::Ran; });
        CHECK(o == RunOutcome::Skipped);
    }
    CHECK(tally.all.load() == 3 && tally.production.load() == 1 && tally.preSr.load() == 1);
    CountedCall(tally, Placement::PreSr, [] { return RunOutcome::Ran; }, [](RunOutcome r) { return r == RunOutcome::Ran; });
    CHECK(tally.all.load() == 3 && tally.preSr.load() == 1); // a run is no fallback
    FallbackTally old;
    old.Note(true);
    old.Note(false);
    CHECK(old.all.load() == 2 && old.production.load() == 1 && old.preSr.load() == 0); // the bool form is unchanged

    // the timer ring attributes a measurement to the mode of the run that wrote it
    TimerRing ring;
    ring.NoteStart();
    ring.NoteSubmitted(5, Placement::PreSr);
    ring.NoteStart();
    ring.NoteSubmitted(6, Placement::PostSr);
    ring.NoteStart();
    ring.NoteSubmitted(7, true); // the bool form: production
    CHECK(ring.CanRead([](uint64_t) { return true; }));
    CHECK(ring.ReadMode() == Placement::PreSr && !ring.ReadIsProduction()); // the slot after the current one: the oldest
    ring.NoteStart();
    ring.NoteSubmitted(8, Placement::BringUp);
    CHECK(ring.ReadMode() == Placement::PostSr && ring.ReadIsProduction());
    ring.NoteStart();
    ring.NoteSubmitted(9, false);
    CHECK(ring.ReadMode() == Placement::PostSr && ring.ReadIsProduction()); // run 7 (bool true) is production
    ring.NoteStart();
    CHECK(ring.ReadMode() == Placement::BringUp && !ring.ReadIsProduction());
}
} // namespace

int main()
{
    TestPlacementFor();
    TestKeyOffIsUnchanged();
    TestEvaluateOutcomes();
    TestNrImageProblemCostsOneFailure();
    TestSlotRenderPreSr();
    TestColourGivenToTheEvaluate();
    TestResetRule();
    TestSourceResetDebounce();
    TestSlotResetReachesTheEvaluate();
    TestPostSrNrStatistic();
    TestTintFor();
    TestShapes();
    TestResultKinds();
    TestInputsFit();
    TestFormatsAndCopies();
    TestPlacementFlip();
    TestLatchesAndCounters();
    return TEST_RESULT("chara_nr_presr");
}
