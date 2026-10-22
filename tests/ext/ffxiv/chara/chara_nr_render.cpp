// Preview NR, the decisions of one render of a preview image, called exactly as CharaDlss.cpp calls them.
//
// A render has two moments in the glue, with effects between them, so the pure code owns the order and the effects are passed in:
//   DecideSlotRender     inside RunSlot: is the production entry offered, which colour does the alpha pass get, which tint, did the slot's
//                        composed picture come from NR
//   DecideFinisherRender at the finisher, after the slot step: the bring-up step, "NR produced a result for this render", and whether the
//                        older NR result of the image is withdrawn
// What this proves: every module outcome other than "Ran with an image" (Preparing, Skipped, Failed, NotAvailable, an image missing, a failed
// alpha pass) leaves the super-resolution output as it is, counts as "NR did not produce a result" and withdraws the older result, while a
// produced image does the opposite; the effects run in the glue's order and only when the rules allow them; and the new functions agree with the
// logic the glue used to carry inline (copied below as the oracle) over a sweep of every input.
// What it cannot prove here: that the glue passes the right fact into each argument (guarded as source text by chara_dlss_glue_text; the facts snapshot of the finisher is taken before the slot
// step, the fresh one inside it) and the one line that carries "composed from NR" from the slot step to the finisher step.
#include "check.h"

#include "ext/ffxiv/chara/CharaNrPlumbing.h"

#include <string>
#include <vector>

using namespace ext::ffxiv::chara::nr;

namespace
{
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
    return f;
}
constexpr int kFactsBits = 128;

constexpr RunOutcome kOutcomes[] = { RunOutcome::Failed, RunOutcome::Ran, RunOutcome::Preparing, RunOutcome::NotAvailable, RunOutcome::Skipped, RunOutcome::Paused };
constexpr RunOutcome kNotRan[] = { RunOutcome::Failed, RunOutcome::Preparing, RunOutcome::NotAvailable, RunOutcome::Skipped, RunOutcome::Paused };
constexpr long kOutcomeCount = sizeof(kOutcomes) / sizeof(kOutcomes[0]);

// The effects of the glue as fakes: they record the order in which they ran and answer what the test says.
struct Effects
{
    std::vector<std::string> log;
    ProductionAnswer answer;
    bool alphaOk = true;
    bool bringUpResult = false;
    AlphaInput alphaInputSeen = AlphaInput::SrOutput;
    TintKind tintSeen = TintKind::None;

    ProductionAnswer Production()
    {
        log.push_back("production");
        return answer;
    }
    bool Alpha(AlphaInput input, TintKind tint)
    {
        log.push_back("alpha");
        alphaInputSeen = input;
        tintSeen = tint;
        return alphaOk;
    }
    bool BringUp()
    {
        log.push_back("bringup");
        return bringUpResult;
    }
};

SlotRender Slot(const PlumbingFacts& facts, bool consumerRecent, bool tintOn, Effects& e)
{
    return DecideSlotRender(facts, consumerRecent, tintOn, Placement::PostSr, false, [&] { return e.Production(); }, [&](AlphaInput i, TintKind t) { return e.Alpha(i, t); });
}

FinisherRender Finisher(const PlumbingFacts& facts, bool accepted, bool consumerRecent, bool srInCharge, bool productionComposed, Effects& e)
{
    return DecideFinisherRender(facts, accepted, consumerRecent, srInCharge, productionComposed, [&] { return e.BringUp(); });
}

// ---- The oracle: the glue's own logic before it moved into the header (CharaDlss.cpp RunSlot and Around at 191c89b2), step by step ------------
struct OldRender
{
    bool offered = false;
    bool produced = false;
    AlphaInput alphaInput = AlphaInput::SrOutput;
    TintKind tint = TintKind::None;
    bool alphaOk = false;
    bool composedFromNr = false;
    bool flag = false; // g_nrProductionThisRender at the end of the slot step
    bool runBringUp = false;
    bool bringUpProduced = false;
    bool nrProduced = false;
    bool invalidate = false;
    std::vector<std::string> log;
};

OldRender Old(const PlumbingFacts& fresh, const PlumbingFacts& snapshot, bool slotStepRuns, bool slotConsumerRecent, bool tintOn,
              const ProductionAnswer& answer, bool alphaOkWanted, bool finisherAccepted, bool finisherConsumerRecent, bool srInCharge,
              bool bringUpResult)
{
    OldRender o;
    // Around, case Finisher: the flag is reset when the finisher is drawn
    o.flag = false;
    if (slotStepRuns)
    {
        // RunSlot, after the SR evaluate succeeded
        bool haveImage = false; // nrOutput != nullptr
        if (RunProductionNow(fresh, true, slotConsumerRecent))
        {
            o.offered = true;
            o.log.push_back("production");
            // RunNrProductionInner returned the image only for Ran with a view
            haveImage = answer.outcome == RunOutcome::Ran && answer.hasView;
        }
        o.produced = haveImage;
        o.alphaInput = ChooseAlphaInput(haveImage);
        o.tint = AlphaTintFor(tintOn, o.alphaInput);
        o.log.push_back("alpha");
        o.alphaOk = alphaOkWanted;
        if (!o.alphaOk)
        {
            // FailSlot and return: composedFromNr stays false, the flag is not set
        }
        else
        {
            o.composedFromNr = o.alphaInput == AlphaInput::NrOutput;
            o.flag = o.composedFromNr;
        }
    }
    // Around, after Finish(): the bring-up step and the withdrawal
    if (RunBringUpNow(snapshot, finisherAccepted, finisherConsumerRecent, srInCharge))
    {
        o.runBringUp = true;
        o.log.push_back("bringup");
        o.bringUpProduced = bringUpResult;
    }
    o.nrProduced = NrProducedForRender(o.bringUpProduced, o.flag);
    o.invalidate = InvalidateNow(snapshot, o.nrProduced);
    return o;
}

// ---- Tests ---------------------------------------------------------------------------------------------------------------------------
// The new functions against the oracle: every combination of the facts the decisions read, the module's answers and the passes' results.
void TestAgainstTheOldGlue()
{
    long cases = 0;
    for (int freshBits = 0; freshBits < kFactsBits; ++freshBits)
        for (int snapshotChange = 0; snapshotChange < 2; ++snapshotChange) // the finisher's snapshot may differ from the fresh facts in nrHoldsResources
            for (int rest = 0; rest < 256; ++rest)
            {
                const bool slotConsumerRecent = (rest & 1) != 0, tintOn = (rest & 2) != 0, alphaOk = (rest & 4) != 0, accepted = (rest & 8) != 0;
                const bool finisherConsumerRecent = (rest & 16) != 0, srInCharge = (rest & 32) != 0, bringUpResult = (rest & 64) != 0;
                const bool hasView = (rest & 128) != 0;
                const PlumbingFacts fresh = FactsOf(freshBits);
                PlumbingFacts snapshot = fresh;
                snapshot.nrHoldsResources = snapshotChange != 0 ? !fresh.nrHoldsResources : fresh.nrHoldsResources;
                for (const RunOutcome outcome : kOutcomes)
                    for (int slotRuns = 0; slotRuns < 2; ++slotRuns)
                    {
                        Effects e;
                        e.answer = { outcome, hasView };
                        e.alphaOk = alphaOk;
                        e.bringUpResult = bringUpResult;
                        const OldRender want = Old(fresh, snapshot, slotRuns != 0, slotConsumerRecent, tintOn, e.answer, alphaOk, accepted,
                                                   finisherConsumerRecent, srInCharge, bringUpResult);

                        bool composed = false; // g_nrProductionThisRender: false when the slot step did not run
                        if (slotRuns != 0)
                        {
                            const SlotRender got = Slot(fresh, slotConsumerRecent, tintOn, e);
                            CHECK(got.offered == want.offered && got.produced == want.produced && got.alphaInput == want.alphaInput &&
                                  got.tint == want.tint && got.alphaOk == want.alphaOk && got.composedFromNr == want.composedFromNr);
                            // the glue sets the flag only when the alpha pass succeeded
                            if (got.alphaOk)
                                composed = got.composedFromNr;
                            CHECK(composed == want.flag);
                        }
                        const FinisherRender fin = Finisher(snapshot, accepted, finisherConsumerRecent, srInCharge, composed, e);
                        CHECK(fin.runBringUp == want.runBringUp && fin.bringUpProduced == want.bringUpProduced &&
                              fin.nrProducedForRender == want.nrProduced && fin.invalidate == want.invalidate);
                        CHECK(e.log == want.log);
                        ++cases;
                    }
            }
    CHECK(cases == static_cast<long>(kFactsBits) * 2 * 256 * kOutcomeCount * 2);
}

// The point of the function: whatever the module answers other than an image, the render keeps the SR output, is not an NR result, and
// withdraws the older NR result of the image.
void TestFailOpen()
{
    PlumbingFacts facts;
    facts.srWanted = facts.nrWanted = true;
    facts.srGateOpen = true;
    facts.nrHoldsResources = true; // the module holds a stream, so there is an older result to withdraw

    for (const bool tintOn : { false, true })
    {
        for (const RunOutcome outcome : kNotRan)
            for (const bool hasView : { false, true })
            {
                Effects e;
                e.answer = { outcome, hasView }; // an image next to a non-Ran outcome is not trusted either
                const SlotRender s = Slot(facts, true, tintOn, e);
                CHECK(s.offered);
                CHECK(e.log == (std::vector<std::string> { "production", "alpha" }));
                CHECK(!s.produced && s.alphaInput == AlphaInput::SrOutput && e.alphaInputSeen == AlphaInput::SrOutput);
                CHECK(s.tint == (tintOn ? TintKind::Sr : TintKind::None) && e.tintSeen == s.tint);
                CHECK(s.alphaOk && !s.composedFromNr);
                // the SR path is in charge of this preview (no bring-up), so nothing produces a result for this render
                const FinisherRender f = Finisher(facts, true, true, /*srInCharge=*/true, s.composedFromNr, e);
                CHECK(!f.runBringUp && !f.nrProducedForRender && f.invalidate);
            }

        // the module said Ran and handed back an image
        Effects ok;
        ok.answer = { RunOutcome::Ran, true };
        const SlotRender s = Slot(facts, true, tintOn, ok);
        CHECK(s.offered && s.produced && s.alphaInput == AlphaInput::NrOutput && ok.alphaInputSeen == AlphaInput::NrOutput);
        CHECK(s.tint == (tintOn ? TintKind::Nr : TintKind::None) && ok.tintSeen == s.tint);
        CHECK(s.alphaOk && s.composedFromNr);
        const FinisherRender f = Finisher(facts, true, true, true, s.composedFromNr, ok);
        CHECK(f.nrProducedForRender && !f.invalidate);

        // Ran, but no image came back: nothing to compose
        Effects noImage;
        noImage.answer = { RunOutcome::Ran, false };
        const SlotRender n = Slot(facts, true, tintOn, noImage);
        CHECK(n.offered && !n.produced && n.alphaInput == AlphaInput::SrOutput && !n.composedFromNr);
        CHECK(!Finisher(facts, true, true, true, n.composedFromNr, noImage).nrProducedForRender);

        // the image was there but the alpha pass failed: the slot fails, the picture is not from NR, the older result is withdrawn
        Effects badAlpha;
        badAlpha.answer = { RunOutcome::Ran, true };
        badAlpha.alphaOk = false;
        const SlotRender b = Slot(facts, true, tintOn, badAlpha);
        CHECK(b.produced && !b.alphaOk && !b.composedFromNr);
        const FinisherRender bf = Finisher(facts, true, true, true, b.composedFromNr, badAlpha);
        CHECK(!bf.nrProducedForRender && bf.invalidate);
    }

    // a production success is an NR result; a bring-up result is one too (the preview the SR path does not serve)
    Effects bring;
    bring.bringUpResult = true;
    const FinisherRender fb = Finisher(facts, true, true, /*srInCharge=*/false, /*productionComposed=*/false, bring);
    CHECK(fb.runBringUp && fb.bringUpProduced && fb.nrProducedForRender && !fb.invalidate);
    bring.bringUpResult = false; // ran and returned without a result
    const FinisherRender fn = Finisher(facts, true, true, false, false, bring);
    CHECK(fn.runBringUp && !fn.bringUpProduced && !fn.nrProducedForRender && fn.invalidate);
    // nothing held by the module: nothing to withdraw
    facts.nrHoldsResources = false;
    CHECK(!Finisher(facts, true, true, false, false, bring).invalidate);
}

// The order of the effects and the rules that allow them.
void TestOrderAndGating()
{
    PlumbingFacts facts;
    facts.srWanted = facts.nrWanted = true;
    facts.srGateOpen = true;

    // production is offered only to a window somebody shows; the alpha pass runs once in every case, after production
    {
        Effects e;
        e.answer = { RunOutcome::Ran, true };
        const SlotRender s = Slot(facts, /*consumerRecent=*/false, false, e);
        CHECK(!s.offered && !s.produced && s.alphaInput == AlphaInput::SrOutput && s.alphaOk);
        CHECK(e.log == (std::vector<std::string> { "alpha" })); // the module was not asked
    }
    {
        Effects e;
        e.answer = { RunOutcome::Ran, true };
        Slot(facts, true, false, e);
        CHECK(e.log == (std::vector<std::string> { "production", "alpha" }));
    }
    // NR latched, switched off or unavailable: the module is not asked
    for (int what = 0; what < 3; ++what)
    {
        PlumbingFacts f = facts;
        f.nrLatched = what == 0;
        f.nrWanted = what != 1;
        f.nrAvailable = what != 2;
        Effects e;
        e.answer = { RunOutcome::Ran, true };
        const SlotRender s = Slot(f, true, false, e);
        CHECK(!s.offered && !s.composedFromNr && e.log == (std::vector<std::string> { "alpha" }));
    }
    // the alpha pass is given the colour decision of this render, never the previous one
    {
        Effects e;
        e.answer = { RunOutcome::Ran, true };
        CHECK(Slot(facts, true, true, e).composedFromNr);
        Effects f;
        f.answer = { RunOutcome::Preparing, false };
        const SlotRender s = Slot(facts, true, true, f);
        CHECK(!s.composedFromNr && f.alphaInputSeen == AlphaInput::SrOutput && f.tintSeen == TintKind::Sr);
    }

    // the finisher: the bring-up runs only when the rules allow it, and its result is read after it ran
    {
        Effects e;
        e.bringUpResult = true;
        // the SR path in charge: no bring-up, even for a shown window and an accepted finisher
        Finisher(facts, true, true, true, false, e);
        CHECK(e.log.empty());
        // a rejected finisher, a window nobody shows
        Finisher(facts, false, true, false, false, e);
        Finisher(facts, true, false, false, false, e);
        CHECK(e.log.empty());
        // a preview the SR path does not serve
        const FinisherRender f = Finisher(facts, true, true, false, false, e);
        CHECK(e.log == (std::vector<std::string> { "bringup" }) && f.nrProducedForRender);
    }
    // the withdrawal reads the facts it is given (held: withdrawn, empty: nothing to withdraw). Which snapshot the glue gives it (the one taken at
    // the top of the dispatcher call) cannot be seen here: chara_dlss_glue_text guards that call.
    {
        Effects e;
        PlumbingFacts held = facts;
        held.nrHoldsResources = true;
        CHECK(Finisher(held, true, true, true, false, e).invalidate);
        PlumbingFacts empty = facts;
        empty.nrHoldsResources = false;
        CHECK(!Finisher(empty, true, true, true, false, e).invalidate);
        // and a production success (carried by the flag) is never withdrawn in the render that made it
        CHECK(!Finisher(held, true, true, true, true, e).invalidate);
    }
}

// The image the alpha pass reads: the NR output exactly when the decision says NrOutput, the SR output otherwise. Two distinct handles, so a swapped
// pair or a missing hand-off cannot give the same answer; and through DecideSlotRender, so the colour follows the module's answer.
void TestAlphaColourFor()
{
    int nrImage = 0, srImage = 0;
    const int* nr = &nrImage;
    const int* sr = &srImage;
    CHECK(AlphaColourFor(AlphaInput::NrOutput, nr, sr) == nr);
    CHECK(AlphaColourFor(AlphaInput::SrOutput, nr, sr) == sr);
    CHECK(AlphaColourFor(AlphaInput::NrOutput, nr, sr) != AlphaColourFor(AlphaInput::SrOutput, nr, sr));

    PlumbingFacts facts;
    facts.srWanted = facts.nrWanted = true;
    facts.srGateOpen = true;
    for (const bool recent : { false, true })
        for (const RunOutcome outcome : kOutcomes)
            for (const bool hasView : { false, true })
            {
                Effects e;
                e.answer = { outcome, hasView };
                const int* given = nullptr;
                const SlotRender r = DecideSlotRender(
                    facts, recent, false, Placement::PostSr, false, [&] { return e.Production(); },
                    [&](AlphaInput input, TintKind)
                    {
                        given = AlphaColourFor(input, nr, sr);
                        return true;
                    });
                const bool nrImageIsUsed = recent && outcome == RunOutcome::Ran && hasView;
                CHECK(r.produced == nrImageIsUsed);
                CHECK(given == (nrImageIsUsed ? nr : sr));
            }
}

void TestConsumerRecentFor()
{
    ConsumerSeen seen;
    int image = 0;
    const void* key = &image;
    // no texture: never recent
    CHECK(!ConsumerRecentFor(seen, nullptr, 1000, 1000));
    // unseen
    CHECK(!ConsumerRecentFor(seen, key, 1000, 1000));
    seen.Record(reinterpret_cast<uintptr_t>(key), 500);
    CHECK(ConsumerRecentFor(seen, key, 1000, 1000));
    CHECK(ConsumerRecentFor(seen, key, 1500, 1000));
    CHECK(!ConsumerRecentFor(seen, key, 1501, 1000)); // older than the window
    // another image is its own key
    int other = 0;
    CHECK(!ConsumerRecentFor(seen, &other, 600, 1000));
    // the same answer as the table's own rule
    CHECK(ConsumerRecentFor(seen, key, 700, 1000) == seen.Recent(reinterpret_cast<uintptr_t>(key), 700, 1000));
}
} // namespace

int main()
{
    TestAgainstTheOldGlue();
    TestFailOpen();
    TestOrderAndGating();
    TestAlphaColourFor();
    TestConsumerRecentFor();
    return TEST_RESULT("chara_nr_render");
}
