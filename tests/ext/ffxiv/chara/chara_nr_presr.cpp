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
constexpr RunOutcome kOutcomes[] = { RunOutcome::Failed, RunOutcome::Ran, RunOutcome::Preparing, RunOutcome::NotAvailable, RunOutcome::Skipped };
constexpr RunOutcome kNotRan[] = { RunOutcome::Failed, RunOutcome::Preparing, RunOutcome::NotAvailable, RunOutcome::Skipped };
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
    for (int bits = 0; bits < kFactsBits; ++bits)
        for (const bool srInCharge : { false, true })
        {
            const PlumbingFacts f = FactsOf(bits);
            // typed independently of the helpers
            const bool srOk = f.srWanted && !f.srLatched && f.srGateOpen;
            const bool nrOk = f.nrWanted && !f.nrLatched && f.nrAvailable;
            const bool before = f.preSrWanted && !f.preSrLatched && srInCharge && srOk && nrOk;
            CHECK(PlacementFor(f, srInCharge) == (before ? Placement::PreSr : Placement::PostSr));
            if (!f.preSrWanted || f.preSrLatched)
                CHECK(PlacementFor(f, srInCharge) == Placement::PostSr); // key off or latched off: after SR, whatever else is true
            for (const bool recent : { false, true })
                for (const Placement p : kPlacements)
                {
                    const bool run = p == Placement::PreSr && recent && srOk && nrOk;
                    CHECK(RunPreSrNow(f, p, recent) == run);
                }
            ++cases;
        }
    CHECK(cases == kFactsBits * 2);
    // each condition alone turns the placement back to after SR
    PlumbingFacts on = BothOn();
    CHECK(PlacementFor(on, true) == Placement::PreSr);
    PlumbingFacts f = on;
    f.preSrWanted = false;
    CHECK(PlacementFor(f, true) == Placement::PostSr);
    f = on;
    f.preSrLatched = true;
    CHECK(PlacementFor(f, true) == Placement::PostSr);
    f = on;
    f.nrWanted = false; // "no effect unless PreviewNR is on"
    CHECK(PlacementFor(f, true) == Placement::PostSr);
    f = on;
    f.srWanted = false; // "... and PreviewDLSS"
    CHECK(PlacementFor(f, true) == Placement::PostSr);
    CHECK(PlacementFor(on, false) == Placement::PostSr);
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

    ProductionAnswer RunPreSr()
    {
        log.push_back("presr");
        return answer;
    }
    bool Evaluate(SrColourSource source)
    {
        log.push_back(source == SrColourSource::PreSr ? "eval-nr" : "eval-plain");
        colours.push_back(SrColourFor<const int*>(source, &nrImage, &plainImage));
        return source == SrColourSource::PreSr ? nrEvaluate : plainEvaluate;
    }
};

SrEvaluation Decide(const PlumbingFacts& f, bool srInCharge, bool recent, Fake& e)
{
    return DecideSrEvaluate(f, srInCharge, recent, [&] { return e.RunPreSr(); }, [&](SrColourSource s) { return e.Evaluate(s); });
}

// Key off, latched off, or not eligible: the pre-SR entry is never called and the evaluate runs exactly once on the plain colour, as it did before.
void TestKeyOffIsUnchanged()
{
    long cases = 0;
    for (int bits = 0; bits < kFactsBits; ++bits)
    {
        const PlumbingFacts f = FactsOf(bits);
        for (const bool srInCharge : { false, true })
            for (const bool recent : { false, true })
                for (const bool plainOk : { false, true })
                {
                    Fake e;
                    e.plainEvaluate = plainOk;
                    e.nrEvaluate = !plainOk; // would be different if it were ever used
                    const SrEvaluation r = Decide(f, srInCharge, recent, e);
                    ++cases;
                    if (r.placement != Placement::PreSr)
                    {
                        CHECK(e.log == (Log { "eval-plain" }));
                        CHECK(e.colours.size() == 1 && e.colours[0] == &e.plainImage);
                        CHECK(r.ok == plainOk && !r.preSrOffered && !r.preSrProduced && !r.usedPreSrColour && !r.latchPreSr);
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
        const SrEvaluation r = Decide(f, true, true, e);
        CHECK(e.log == (Log { "presr", "eval-nr" }));
        CHECK(e.colours.size() == 1 && e.colours[0] == &e.nrImage);
        CHECK(r.placement == Placement::PreSr && r.preSrOffered && r.preSrProduced && r.usedPreSrColour && r.ok && !r.latchPreSr);
    }
    // the evaluate refuses the NR image: run again on the plain colour in the same render, latch once, the NR image is not used
    {
        Fake e;
        e.nrEvaluate = false;
        const SrEvaluation r = Decide(f, true, true, e);
        CHECK(e.log == (Log { "presr", "eval-nr", "eval-plain" }));
        CHECK(e.colours.size() == 2 && e.colours[0] == &e.nrImage && e.colours[1] == &e.plainImage);
        CHECK(r.preSrProduced && !r.usedPreSrColour && r.ok && r.latchPreSr);
        // with the latch set the next render is after SR and never calls the entry
        PlumbingFacts latched = f;
        latched.preSrLatched = true;
        Fake next;
        const SrEvaluation n = Decide(latched, true, true, next);
        CHECK(n.placement == Placement::PostSr && next.log == (Log { "eval-plain" }) && !n.latchPreSr);
    }
    // both evaluates fail: the render fails as it always did (the caller fails the slot); the colour was not the cause, so nothing is latched
    {
        Fake e;
        e.nrEvaluate = false;
        e.plainEvaluate = false;
        const SrEvaluation r = Decide(f, true, true, e);
        CHECK(e.log == (Log { "presr", "eval-nr", "eval-plain" }));
        CHECK(!r.ok && !r.latchPreSr && !r.usedPreSrColour);
    }
    // the pre-SR run gave no image, whatever the reason: the evaluate runs once on the plain colour, nothing is latched
    for (const RunOutcome outcome : kNotRan)
        for (const bool hasImage : { false, true })
        {
            Fake e;
            e.answer = { outcome, hasImage }; // an image next to a non-Ran outcome is not trusted
            e.nrEvaluate = false;
            const SrEvaluation r = Decide(f, true, true, e);
            CHECK(e.log == (Log { "presr", "eval-plain" }));
            CHECK(e.colours[0] == &e.plainImage);
            CHECK(r.placement == Placement::PreSr && r.preSrOffered && !r.preSrProduced && !r.usedPreSrColour && r.ok && !r.latchPreSr);
        }
    // Ran without an image: a defect of the module; plain colour this render, latched off (one line), not another try every render
    {
        Fake e;
        e.answer = { RunOutcome::Ran, false };
        const SrEvaluation r = Decide(f, true, true, e);
        CHECK(e.log == (Log { "presr", "eval-plain" }));
        CHECK(!r.preSrProduced && r.ok && r.latchPreSr);
        Fake both;
        both.answer = { RunOutcome::Ran, false };
        both.plainEvaluate = false;
        CHECK(!Decide(f, true, true, both).latchPreSr); // the render failed for another reason
    }
    // a window nobody shows: the entry is not called
    {
        Fake e;
        const SrEvaluation r = Decide(f, true, false, e);
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
        const SrEvaluation r = Decide(n, true, true, e);
        CHECK(r.placement == Placement::PostSr && e.log == (Log { "eval-plain" }));
    }
}

// The glue's evaluate, recorded through the real ApplyEvaluate (the CharaDlssNgx.h pattern): the same lambda body as RunSlot's, with a fake in place of
// the NGX parameter object. Color is the NR result when the pre-SR run produced one and the evaluate took it, the plain colour otherwise; the retry
// differs from the first evaluate in Color only.
struct NgxRecorder
{
    const void* colour = nullptr;
    std::vector<std::string> others; // every other value set by this evaluate, in order
    void SetUInt(const char* n, uint32_t v) { others.push_back(std::string(n) + "=" + std::to_string(v)); }
    void SetInt(const char* n, int v) { others.push_back(std::string(n) + "=" + std::to_string(v)); }
    void SetFloat(const char* n, float v) { others.push_back(std::string(n) + "=" + std::to_string(v)); }
    void SetResource(const char* n, const void* r)
    {
        if (std::string(n) == ngx::kColor)
            colour = r;
        else
            others.push_back(std::string(n) + "@" + std::to_string(reinterpret_cast<uintptr_t>(r)));
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
    in.reset = true;
    struct Case
    {
        bool keyOn;
        ProductionAnswer answer;
        bool nrEvaluate;
        std::vector<const void*> want; // the Color of each evaluate, in order
        bool latch;
    };
    const Case cases[] = {
        { true, { RunOutcome::Ran, true }, true, { &nrImage }, false },               // NR ran, the evaluate took it
        { true, { RunOutcome::Ran, true }, false, { &nrImage, &plainImage }, true },  // refused: again on the plain colour, latch
        { true, { RunOutcome::Preparing, false }, true, { &plainImage }, false },     // no image yet
        { true, { RunOutcome::Failed, false }, true, { &plainImage }, false },
        { true, { RunOutcome::Skipped, true }, true, { &plainImage }, false },        // an image next to a non-Ran outcome is not used
        { false, { RunOutcome::Ran, true }, true, { &plainImage }, false },           // key off: never the NR image
    };
    for (const Case& c : cases)
    {
        PlumbingFacts f = BothOn();
        f.preSrWanted = c.keyOn;
        std::vector<NgxRecorder> recorded;
        const SrEvaluation r = DecideSrEvaluate(
            f, true, true, [&] { return c.answer; },
            [&](SrColourSource source)
            {
                NgxRecorder rec;
                // RunSlot's lambda: Color is SrColourFor(source, the NR result, g.color); the rest does not depend on it
                ngx::ApplyEvaluate<NgxRecorder, const void*>(rec, in, SrColourFor<const void*>(source, &nrImage, &plainImage), &outImage, &depthImage,
                                                              &motionImage);
                recorded.push_back(rec);
                return source == SrColourSource::PreSr ? c.nrEvaluate : true;
            });
        CHECK(recorded.size() == c.want.size());
        for (size_t i = 0; i < recorded.size() && i < c.want.size(); ++i)
        {
            CHECK(recorded[i].colour == c.want[i]);
            CHECK(recorded[i].others == recorded[0].others); // the retry carries the same jitter, reset, guides and output
        }
        CHECK(r.ok && r.latchPreSr == c.latch);
    }
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
    TestSlotRenderPreSr();
    TestColourGivenToTheEvaluate();
    TestTintFor();
    TestShapes();
    TestResultKinds();
    TestInputsFit();
    TestFormatsAndCopies();
    TestPlacementFlip();
    TestLatchesAndCounters();
    return TEST_RESULT("chara_nr_presr");
}
