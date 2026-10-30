// [HudLess] HdrPaste: the producer step of an HDR-output present. Under HDR output the paste image is built from a PQ copy of the
// final frame, and that copy needs the slot's HDR stage (resources and command list). The stage used to be created only by the
// readiness check (which needs a finished paste image, so it never ran first) and by probing presents, so on the first non-probing
// present the producer ran without a stage, built nothing, and no image ever completed (inferred from reading the code, never observed
// in a game). With HdrPaste = on the stage is ensured before
// the producer on every HDR present; with off (the default) the order is exactly the old one, and that path is not tried in a game
// yet. The decision is the pure function the engine calls; the engine's call site is pinned by text.
#include "ext/hudless/HudlessLogic.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

using namespace ext::hudless;

namespace
{
struct FakeSlot
{
    bool stage = false;       // the HDR stage of the slot exists
    bool image = false;       // a completed paste image exists
    std::vector<std::string> order;
};

// What the engine's Post does with the producer on a present whose action is Produce, and what its readiness check asks first
struct FakeEngine
{
    bool hdr = false;
    bool hdrPaste = false;
    int stageFailures = 0;   // the next creations of the stage that fail
    int stageAttempts = 0;   // creations tried (each one recreates resources and logs an error in the engine)
    int latchedLogs = 0;     // times the engine would write the "not trying again" line
    HdrStageLatch latch;
    FakeSlot slot;

    // Engine::EnsureHdrStageOnce
    bool Ensure()
    {
        return EnsureStageLatched(
            latch, hdr, hdrPaste,
            [&]
            {
                ++stageAttempts;
                if (stageFailures > 0)
                {
                    --stageFailures;
                    return false;
                }
                slot.order.push_back("stage");
                slot.stage = true;
                return true;
            },
            [&] { ++latchedLogs; });
    }

    // Engine::PasteReady: a completed image is needed first; then (HDR) the stage is ensured
    bool PasteReady()
    {
        if (!slot.image)
            return false;
        if (!hdr)
            return true;
        slot.order.push_back("stage");
        slot.stage = true;
        return true;
    }

    // Engine::Post
    bool Produce(bool probing)
    {
        latch.Observe(hdr, hdrPaste); // every present that produces, SDR ones included
        return ProduceWithHdrStage(
            hdr, probing, hdrPaste, [&] { return Ensure(); },
            [&]
            {
                slot.order.push_back("produce");
                if (hdr && !slot.stage)
                    return false; // BuildHdrFinal finds no allocator and returns null
                slot.image = true;
                return true;
            });
    }
};

void TestDecision()
{
    // off: exactly the old rule, the stage first only on a probing present under HDR
    assert(!EnsureHdrStageBeforeProduce(false, false, false));
    assert(!EnsureHdrStageBeforeProduce(false, true, false));
    assert(!EnsureHdrStageBeforeProduce(true, false, false)); // the deadlock: nothing creates the stage before the first image
    assert(EnsureHdrStageBeforeProduce(true, true, false));
    // on: always under HDR output, never otherwise
    assert(!EnsureHdrStageBeforeProduce(false, false, true));
    assert(!EnsureHdrStageBeforeProduce(false, true, true));
    assert(EnsureHdrStageBeforeProduce(true, false, true));
    assert(EnsureHdrStageBeforeProduce(true, true, true));
}

void TestCycle()
{
    // off, HDR, non-probing presents: the producer runs without a stage, no image ever completes, the readiness check never passes
    {
        FakeEngine e;
        e.hdr = true;
        for (int n = 0; n < 50; ++n)
        {
            assert(!e.PasteReady());
            assert(!e.Produce(false));
        }
        assert(!e.slot.image && !e.slot.stage);
        assert(e.slot.order.size() == 50 && e.slot.order[0] == "produce"); // the old order: produce, never a stage first
    }
    // on, HDR: the stage first, then the image; the next present is ready
    {
        FakeEngine e;
        e.hdr = true;
        e.hdrPaste = true;
        assert(!e.PasteReady());
        assert(e.Produce(false));
        assert((e.slot.order == std::vector<std::string> { "stage", "produce" }));
        assert(e.PasteReady());
    }
    // a probing present under HDR creates the stage first with either setting (the old behaviour is kept)
    for (const bool paste : { false, true })
    {
        FakeEngine e;
        e.hdr = true;
        e.hdrPaste = paste;
        assert(e.Produce(true));
        assert((e.slot.order == std::vector<std::string> { "stage", "produce" }));
    }
    // SDR output: the stage is never touched, whatever the key says
    for (const bool paste : { false, true })
        for (const bool probing : { false, true })
        {
            FakeEngine e;
            e.hdrPaste = paste;
            assert(e.Produce(probing));
            assert((e.slot.order == std::vector<std::string> { "produce" }));
        }
    // a stage that cannot be made stops the producer before it runs (a failed present, not a half-built image)
    {
        bool produced = false;
        const bool ok = ProduceWithHdrStage(
            true, false, true, [] { return false; },
            [&]
            {
                produced = true;
                return true;
            });
        assert(!ok && !produced);
    }
}

// A stage that cannot be made is not retried on every present (each retry recreates resources and writes an error): with HdrPaste = on
// the failure is latched until the HDR output state or the setting changes. With off nothing changes.
void TestFailureLatch()
{
    // on, HDR, the creation keeps failing: one attempt, one line, the producer never runs, however many presents
    {
        FakeEngine e;
        e.hdr = true;
        e.hdrPaste = true;
        e.stageFailures = 1000;
        for (int n = 0; n < 50; ++n)
            assert(!e.Produce(n % 2 == 0)); // probing or not
        assert(e.stageAttempts == 1 && e.latchedLogs == 1);
        assert(e.slot.order.empty() && !e.slot.image);
    }
    // ... the HDR output state changing (to SDR and back) lets it try again, and then it works
    {
        FakeEngine e;
        e.hdr = true;
        e.hdrPaste = true;
        e.stageFailures = 1;
        assert(!e.Produce(false) && !e.Produce(false));
        assert(e.stageAttempts == 1 && e.latchedLogs == 1);
        e.hdr = false;
        assert(e.Produce(false)); // SDR: no stage involved
        e.hdr = true;
        assert(e.Produce(false));
        assert(e.stageAttempts == 2 && e.latchedLogs == 1);
        assert((e.slot.order == std::vector<std::string> { "produce", "stage", "produce" }));
    }
    // ... and so does the setting going off and on again
    {
        FakeEngine e;
        e.hdr = true;
        e.hdrPaste = true;
        e.stageFailures = 1;
        assert(!e.Produce(false));
        e.hdrPaste = false;
        assert(!e.Produce(false)); // off: the old order, no stage before the first image, and the latch is not consulted
        assert(e.stageAttempts == 1);
        e.hdrPaste = true;
        assert(e.Produce(false));
        assert(e.stageAttempts == 2);
    }
    // a second failure after a recovery latches again and writes its line again
    {
        FakeEngine e;
        e.hdr = true;
        e.hdrPaste = true;
        e.stageFailures = 1;
        assert(!e.Produce(false));
        e.hdr = false;
        e.Produce(false);
        e.hdr = true;
        e.stageFailures = 1;
        assert(!e.Produce(false) && !e.Produce(false));
        assert(e.stageAttempts == 2 && e.latchedLogs == 2);
    }
    // off: exactly the old behaviour: a probing present under HDR retries every time, a non-probing one never tries
    {
        FakeEngine e;
        e.hdr = true;
        e.stageFailures = 1000;
        for (int n = 0; n < 20; ++n)
            assert(!e.Produce(true));
        assert(e.stageAttempts == 20 && e.latchedLogs == 0);
        for (int n = 0; n < 20; ++n)
            e.Produce(false);
        assert(e.stageAttempts == 20);
    }
    // the latch on its own
    {
        HdrStageLatch latch;
        assert(latch.MayTry(true, true));
        assert(latch.NoteFailure(true, true));  // the first failure since it was clear
        assert(!latch.NoteFailure(true, true)); // not again
        assert(!latch.MayTry(true, true));
        assert(latch.MayTry(true, false));      // another setting: a fresh key
        latch.Observe(true, true);
        latch.NoteFailure(true, true);
        latch.Clear();
        assert(latch.MayTry(true, true));
    }
}

std::string ReadFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// The text without comments and white space
std::string Squash(const std::string& text)
{
    std::string out;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '/')
        {
            while (i < text.size() && text[i] != '\n')
                ++i;
            continue;
        }
        if (text[i] != ' ' && text[i] != '\t' && text[i] != '\r' && text[i] != '\n')
            out += text[i];
    }
    return out;
}

size_t Count(const std::string& hay, const std::string& needle)
{
    size_t n = 0;
    for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1))
        ++n;
    return n;
}

void TestEngineCallSite(const std::string& repo)
{
    const std::string engine = Squash(ReadFile(repo + "/OptiScaler/ext/hudless/HudlessEngine.cpp"));
    assert(!engine.empty());
    // The producer step goes through the tested function, with the key
    const std::string call = Squash("ProduceWithHdrStage(HdrOutput(), _core.Probing(), ext::Cfg().hudless.HdrPasteOn(),");
    assert(Count(engine, call) == 1);
    assert(engine.find(Squash("[&] { return EnsureHdrStageOnce(index); }")) != std::string::npos);
    // The failure latch: observed on every producing present before the call, used by the one function that creates the stage for the
    // producer, cleared with the ring
    const std::string observe = Squash("_hdrLatch.Observe(HdrOutput(), ext::Cfg().hudless.HdrPasteOn());");
    assert(Count(engine, observe) == 1 && engine.find(observe) < engine.find(call));
    assert(Count(engine, Squash("EnsureStageLatched(_hdrLatch,")) == 1);
    assert(Count(engine, Squash("_hdrLatch.Clear();")) == 1);
    assert(engine.find(Squash("[&] { return ProducePaste(sc, slot, finalImage, fg); }")) != std::string::npos);
    // ... instead of the old inline rule, which never ensured the stage before the first image
    assert(engine.find(Squash("!_core.Probing() || !HdrOutput() || EnsureHdrStage(index)")) == std::string::npos);
    // The readiness check is unchanged: it still ensures the stage, after the image check
    assert(engine.find(Squash("return !HdrOutput() || EnsureHdrStage(static_cast<size_t>(_stagedSlot));")) != std::string::npos);
    // ProducePaste is called from nowhere else
    assert(Count(engine, "ProducePaste(sc,slot,finalImage,fg)") == 1);
    // The latch is keyed by the two facts the producer step uses (a wrong key would latch the failure with the setting off, never latch
    // it, or forget it on every present)
    assert(Count(engine, Squash("EnsureStageLatched(_hdrLatch, HdrOutput(), ext::Cfg().hudless.HdrPasteOn(),")) == 1);
    // The key [HudLess] GuardMode reaches the core through the engine, and the core is told which tags the present sent
    assert(Count(engine, Squash("GuardPolicy CoverageGuardPolicy() override { return ext::Cfg().hudless.GetGuardPolicy(); }")) == 1);
    // ... and so do the two options of that guard policy
    assert(Count(engine, Squash("int CoverageGuardReleaseHolds() override { return ext::Cfg().hudless.ReleaseHolds(); }")) == 1);
    assert(Count(engine, Squash("bool CoverageGuardTripOnAnchorLater() override { return ext::Cfg().hudless.TripOnAnchorLater(); }")) == 1);
    assert(Count(engine, Squash("_core.NoteTagSet(static_cast<uint8_t>((tagged.hudlessTagged ? 1 : 0) | (tagged.uiTagged ? 2 : 0)));")) == 1);
}
} // namespace

int main(int argc, char** argv)
{
    TestDecision();
    TestCycle();
    TestFailureLatch();
    TestEngineCallSite(argc > 1 ? argv[1] : ".");
    std::puts("PASS: HdrPaste off keeps the old producer order, on ensures the HDR stage first (decision, cycle, engine call site)");
    return 0;
}
