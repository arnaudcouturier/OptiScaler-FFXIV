// The native nameplates end to end: the product's game side (PlateNative.cpp) and the product's engine (UiPlates.cpp)
// compiled together and driven through the real call sequence - the game's draw, the present seam, the output pass.
//
// What only this test can show: the classifier and the engine agree about a frame (a frame the classifier refuses is
// never hidden and its image is never drawn over plates the game drew itself); a UI draw inside an already hidden frame
// stays native while the frame's plate image is still drawn; a frame with no plate draw at all clears the drawable image;
// and the ledger rules hold when every decision comes from the real draw state rather than from a reservation the test
// made up.
#include "ext/ExtFeatures.h"
#include "ext/fg/dlssg/PresetStatus.h"
#include "ext/hudless/HudlessConfig.h"
#include "ext/hudless/HudlessEngine.h"

#include "../../hudless/plate_harness.h"

#include "ext/ffxiv/plates/PlateNative.h"

#include <cstdio>

using namespace ext::hudless;
using ext::ffxiv::plates::Snapshot;
using ext::hudless::plates::FrameMode;

namespace
{
int failures = 0;

void Check(bool ok, const char* what, int line)
{
    if (!ok)
    {
        std::printf("FAIL line %d: %s\n", line, what);
        ++failures;
    }
}
#define CHECK(x) Check((x), #x, __LINE__)

const char* ModeName(FrameMode mode)
{
    switch (mode)
    {
    case FrameMode::Hidden:
        return "hidden";
    case FrameMode::Shadow:
        return "shadow";
    default:
        return "declined";
    }
}

// A clean engine for the next scenario: released, key on (endpoint), a bit sink the scenario reads, DLSS-G, SDR.
struct Scenario
{
    plate_test::Driver driver;
    ext::hudless::plates::Counters base;

    explicit Scenario(unsigned multiplier = 3) : driver(64, 32, multiplier)
    {
        ext::hudless::plates::OnRelease();
        driver.Interp(PlateInterp::Endpoint);
        PublishPlateSettings({ PlateInterp::Endpoint, 1, 24 });
        ext::hudless::plates::SetPlateFeatureBitSink(&plate_test::RecordBit);
        plate_test::BitCalls().clear();
        plate_test_clock::Reset();
        State::Instance().isShuttingDown = false;
        State::Instance().activeFgOutput = FGOutput::DLSSG;
        ext::hudless::TestMode() = ext::hudless::Mode::Off;
        ext::hudless::TestRecomposition().enabled = false;
        ext::fg::preset::TestDecision().droppedByPreset = false;
        base = driver.Counters();
    }

    ~Scenario()
    {
        ext::hudless::plates::SetPlateFeatureBitSink(nullptr);
        ext::hudless::plates::OnRelease();
        PublishPlateSettings({ PlateInterp::Off, 1, 24 });
        ext::SetActive(ext::F::Plate, false);
        ext::hudless::plates::sync::ClearViolations();
    }

    ext::hudless::plates::Counters Delta() const
    {
        const auto now = driver.Counters();
        auto delta = now;
        delta.promoted -= base.promoted;
        delta.hidden -= base.hidden;
        delta.shadow -= base.shadow;
        delta.native -= base.native;
        delta.declined -= base.declined;
        delta.slotsCreated -= base.slotsCreated;
        delta.plated -= base.plated;
        delta.skipped -= base.skipped;
        delta.hiddenSkipped -= base.hiddenSkipped;
        delta.pinsOutstanding = now.pinsOutstanding;
        return delta;
    }
};

// One lockstep frame: the game draws (through the classifier), the present seam, then the batch's outputs.
void Frame(Scenario& s, unsigned& n, unsigned outputs = 3, bool withPlate = true, const char* firstDraw = nullptr,
           const char* secondDraw = nullptr)
{
    s.driver.Frame(n, withPlate, firstDraw, secondDraw);
    s.driver.Post();
    for (unsigned k = 0; k < outputs; ++k)
        s.driver.Output(n);
    ++n;
}

// Four warm-up frames: no slot yet, then three shadow frames.
void WarmUp(Scenario& s, unsigned& n, unsigned outputs = 3)
{
    Frame(s, n, outputs); // declined: no slot yet
    Frame(s, n, outputs); // shadow 1
    Frame(s, n, outputs); // shadow 2
    Frame(s, n, outputs); // shadow 3: armed at the end of it
}

// Frames until one hides (at most `limit`). Returns the number of frames run.
int UntilHidden(Scenario& s, unsigned& n, unsigned outputs = 3, int limit = 10)
{
    for (int i = 0; i < limit; ++i)
    {
        Frame(s, n, outputs);
        if (s.driver.Mode(n - 1) == FrameMode::Hidden)
            return i + 1;
    }
    return limit;
}

void ExpectDrawn(const std::vector<plate_test::OutputEntry>& outputs, size_t from, size_t to, unsigned n)
{
    for (size_t i = from; i < to; ++i)
        CHECK(outputs[i].drawn == n);
}

void ExpectNothing(const std::vector<plate_test::OutputEntry>& outputs, size_t from, size_t to)
{
    for (size_t i = from; i < to; ++i)
        CHECK(outputs[i].drawn == 0);
}

// The ledger rules every scenario ends with.
void Common(const Scenario& s, const char* what)
{
    (void) what;
    CHECK(s.driver.Counters().pinsOutstanding == 0);
    CHECK(ext::hudless::plates::sync::Violations() == 0);
    // L1: the pass never draws a record whose frame the game drew natively.
    for (const auto& output : s.driver.Outputs())
    {
        if (output.drawn == 0)
            continue;
        bool native = true;
        for (const auto& frame : s.driver.Frames())
            if (frame.n == output.drawn)
                native = frame.mode != FrameMode::Hidden;
        CHECK(!native);
    }
}

// P1: with the real classifier in the loop, the steady state is the same as the engine's: hidden frames, their image on
// every output, and no native plate draw.
void SteadyStateThroughTheClassifier()
{
    Scenario s;
    unsigned n = 1;
    WarmUp(s, n);
    CHECK(s.driver.Mode(1) == FrameMode::Declined); // no slot on the first present
    for (unsigned f = 2; f <= 4; ++f)
    {
        CHECK(s.driver.Mode(f) == FrameMode::Shadow);
        CHECK(s.driver.NativeDraws(f) == 1);
    }
    ExpectNothing(s.driver.Outputs(), 0, 12);
    const size_t warmOutputs = s.driver.Outputs().size();
    const unsigned firstHidden = n;
    for (unsigned i = 0; i < 12; ++i)
        Frame(s, n);
    for (unsigned f = firstHidden; f < n; ++f)
    {
        CHECK(s.driver.Mode(f) == FrameMode::Hidden);
        CHECK(s.driver.NativeDraws(f) == 0); // the classifier took every plate draw of a hidden frame
    }
    for (size_t i = warmOutputs; i + 3 <= s.driver.Outputs().size(); i += 3)
        ExpectDrawn(s.driver.Outputs(), i, i + 3, s.driver.Outputs()[i].batch);
    // The game side agrees: every draw of those frames matched the rule and was hidden.
    const auto game = Snapshot();
    CHECK(game.drawsHidden > 0);
    CHECK(game.drawsFallback == 0);
    CHECK(s.Delta().hiddenSkipped == 0);
    Common(s, "P1");
    std::puts("    P1 steady state through the classifier: every plate draw matched, no output saw a native frame's image");
}

// P2: a frame whose draw the classifier refuses (a UI draw with another pixel shader) is never hidden, and its own
// native draw is not covered by an older hidden image.
void RefusedDrawPublishesNoStaleImage()
{
    Scenario s;
    unsigned n = 1;
    WarmUp(s, n);
    UntilHidden(s, n, 3);
    CHECK(s.driver.Mode(n - 1) == FrameMode::Hidden);
    const size_t before = s.driver.Outputs().size();
    // The frame draws a UI draw whose state is wrong in one condition: no nameplate pixel shader.
    s.driver.Frame(n, true, "shader", nullptr);
    s.driver.Post();
    for (unsigned k = 0; k < 3; ++k)
        s.driver.Output(n);
    CHECK(s.driver.Mode(n) == FrameMode::Declined); // the first draw was refused, so nothing was reserved
    CHECK(s.driver.NativeDraws(n) == 1);            // the draw is the game's own
    ExpectNothing(s.driver.Outputs(), before, s.driver.Outputs().size());
    ++n;
    // The next frame hides again: the refused frame did not wedge the latch or the gate.
    for (int i = 0; i < 6 && s.driver.Mode(n - 1) != FrameMode::Hidden; ++i)
        Frame(s, n);
    CHECK(s.driver.Mode(n - 1) == FrameMode::Hidden);
    Common(s, "P2");
    std::puts("    P2 refused draw: the frame stays native and no older hidden image is drawn over it");
}

// P3: a UI draw inside an already hidden frame is drawn natively (the documented bound: that draw appears twice), and
// the frame's plate image is still drawn.
void RefusedDrawInsideAHiddenFrame()
{
    Scenario s;
    unsigned n = 1;
    WarmUp(s, n);
    UntilHidden(s, n, 3);
    const size_t before = s.driver.Outputs().size();
    const auto gameBefore = Snapshot();
    Frame(s, n, 3, true, nullptr, "shader"); // the plate draw (hidden) then a wrong UI draw
    CHECK(s.driver.Mode(n - 1) == FrameMode::Hidden);
    CHECK(s.driver.NativeDraws(n - 1) == 1); // the UI draw went to the game's own target
    ExpectDrawn(s.driver.Outputs(), before, s.driver.Outputs().size(), n - 1);
    const auto gameAfter = Snapshot();
    CHECK(gameAfter.drawsHidden == gameBefore.drawsHidden + 1);
    CHECK(gameAfter.drawsFallback == gameBefore.drawsFallback); // the refused draw never reached the redirect
    Common(s, "P3");
    std::puts("    P3 a UI draw inside a hidden frame stays native, and the frame's plate image is still drawn");
}

// P4: a cutscene (no plate draw at all) takes the image away at once, and the plates never come back as a ghost when the
// draws return.
void NoPlateDraw()
{
    Scenario s;
    unsigned n = 1;
    WarmUp(s, n);
    UntilHidden(s, n, 3);
    const size_t before = s.driver.Outputs().size();
    for (unsigned i = 0; i < 6; ++i)
        Frame(s, n, 3, false);
    for (unsigned f = n - 6; f < n; ++f)
        CHECK(s.driver.Mode(f) == FrameMode::Declined);
    ExpectNothing(s.driver.Outputs(), before, s.driver.Outputs().size());
    // The plates come back: native at first (the gate has to re-prove itself), never a ghost of the pre-gap image.
    const size_t back = s.driver.Outputs().size();
    for (unsigned i = 0; i < 6; ++i)
        Frame(s, n);
    for (size_t i = back; i < s.driver.Outputs().size(); ++i)
    {
        if (s.driver.Outputs()[i].drawn == 0)
            continue;
        bool native = true;
        for (const auto& frame : s.driver.Frames())
            if (frame.n == s.driver.Outputs()[i].drawn)
                native = frame.mode != FrameMode::Hidden;
        CHECK(!native);
    }
    Common(s, "P4");
    std::puts("    P4 no plate draw: the image goes away at once and nothing stale comes back");
}
} // namespace

int main()
{
    SteadyStateThroughTheClassifier();
    RefusedDrawPublishesNoStaleImage();
    RefusedDrawInsideAHiddenFrame();
    NoPlateDraw();
    if (failures == 0)
        std::puts("PASS: plate scenarios: the real classifier and the real engine agree on every frame, the ledger rules "
                  "hold, no stale image, no lost plate");
    else
        std::printf("FAILED: %d check(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
