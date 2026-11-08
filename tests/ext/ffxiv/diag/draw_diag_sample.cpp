// The draw diagnostic's constant-buffer sampling: the reads a capture takes of the light and camera constants are limited to one a
// second. A read is due when nothing has been read yet (a fresh session), and when the last read is at least one second old; a draw
// inside that second is not due and must not move the clock forward, or the next draw would be due again and a busy frame would stall
// the render thread once per draw with a staging copy and a map.
//
// The rule lives in the diagnostic's runtime next to the clock it reads, so this test builds that file (see the recipe) and drives the
// rule with the process's own performance counter: the alternative, a copy of the rule in the test, would keep passing after the
// product changed.
#include "check.h"

#include "ext/ffxiv/diag/FfxivDrawDiag.cpp" // the runtime under test: the sample rule and the clock it reads
// The runtime also reads the engine pass module's counters, the screen-space reflections' and the puddles' two stages for its capture folder; this
// test has no module, so every read answers "off".
namespace ext::ffxiv::engine
{
Diagnostics CurrentDiagnostics() { return Diagnostics {}; }
}

namespace ext::ffxiv::engine::ssr
{
Diagnostics CurrentDiagnostics() { return Diagnostics {}; }
FrameObservation CurrentFrameObservation() { return {}; }
void OnRenderCallback(ID3D11DeviceContext*) {}
void OnDiagnosticCaptureStart() {}
}

// The puddles module's two stages answer the same way: the card is off, so engine.txt's block says so and nothing else is read.
namespace ext::ffxiv::engine::gloss
{
Diagnostics CurrentDiagnostics() { return Diagnostics {}; }
ToneDiagnostics CurrentToneDiagnostics() { return ToneDiagnostics {}; }
FrameObservation CurrentFrameObservation() { return {}; }
ToneFrameObservation CurrentToneFrameObservation() { return {}; }
}

namespace ext::present { uint64_t FrameCount() noexcept { return 1; } }

int main()
{
    using namespace ext::ffxiv::diag;

    // The interval the promise is made of, and a clock that can express it.
    CHECK(kSampleSeconds == 1.0); // the ini documentation says once a second
    const int64_t ticksPerSecond = QpcFrequency();
    CHECK(ticksPerSecond > 0);

    // A draw that is not the light pass does not spend the second: the shape is checked first, and only the draw that passes it stamps the clock.
    // Otherwise a frame's first wetness volume would take the slot every second and the light pass would never be sampled, which is the defect that
    // made a whole session's cb.csv report another light's direction and view matrix.
    {
        State sampling;
        CHECK(SampleWanted(sampling)); // nothing read yet
        CHECK(!IsLightPassShape(1, 28, true, true)); // the wetness volume's shape: one R8 target, blended
        CHECK(SampleWanted(sampling));               // passed over, the slot is still open
        CHECK(IsLightPassShape(2, kLightBufferFormat, true, false)); // the light pass: the pair, not blended
        NoteSampled(sampling);
        CHECK(!SampleWanted(sampling)); // inside the same second: a second draw of the light pass waits
        CHECK(!SampleDue(sampling));
    }

    State state;

    // Nothing has been read yet: the session's first draw samples, and it stamps the clock it sampled at.
    CHECK(SampleDue(state));
    const int64_t stamped = state.lastReadQpc;
    CHECK(stamped > 0); // the clock of that read is remembered

    // The next draw, in the same second, is not due and does not move the clock: a refused read is not a read.
    CHECK(!SampleDue(state));
    CHECK_EQ(state.lastReadQpc, stamped);

    // Half a second later it is still inside the interval, and a refused read still leaves the clock alone.
    state.lastReadQpc = Qpc() - ticksPerSecond / 2;
    const int64_t half = state.lastReadQpc;
    CHECK(!SampleDue(state));
    CHECK_EQ(state.lastReadQpc, half);

    // A second after the last read it is due again, and the new read restamps the clock ...
    state.lastReadQpc = Qpc() - ticksPerSecond;
    CHECK(SampleDue(state));
    const int64_t restamped = state.lastReadQpc;
    CHECK(restamped > 0);

    // ... so the draws that follow it wait once more instead of sampling on every draw.
    CHECK(!SampleDue(state));
    CHECK_EQ(state.lastReadQpc, restamped);

    // A zero clock means "never read": the first draw after the config armed the diagnostic samples.
    state.lastReadQpc = 0;
    CHECK(SampleDue(state));
    CHECK(state.lastReadQpc > 0);

    return Finish("draw_diag_sample");
}
