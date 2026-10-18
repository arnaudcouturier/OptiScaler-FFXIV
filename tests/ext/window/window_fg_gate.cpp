// The "DLSS-G is confirmed off" gate: one COMPLETE game present that started and ended with frame generation inactive,
// counted on a fake frame counter. Only for the DLSS-G output.
#include "window_test.h"

using namespace ext::window;

namespace
{
const FgSnapshot kNone { false, false, false };
const FgSnapshot kActive { true, true, false };
const FgSnapshot kPaused { true, false, true };
const FgSnapshot kOff { true, false, false };

// One game present: entry state, exit state.
void Present(FgOffGate& gate, const FgSnapshot& entry, const FgSnapshot& exit)
{
    gate.OnPresentBegin(entry);
    gate.OnPresentEnd(exit);
}
} // namespace

int main()
{
    // Never confirmed before any present.
    {
        FgOffGate gate;
        CHECK(!gate.Confirmed(true));
        CHECK(gate.Count() == 0);
    }

    // One complete present with the feature paused (inactive) confirms; the same count does not confirm a non-DLSS-G output.
    {
        FgOffGate gate;
        Present(gate, kPaused, kPaused);
        CHECK(gate.Confirmed(true));
        CHECK(!gate.Confirmed(false)); // only the DLSS-G output has this stall
        CHECK(gate.Count() == 1);
    }

    // The present during which frame generation was still active does not count: it started with the feature active.
    {
        FgOffGate gate;
        Present(gate, kActive, kOff); // Deactivate happened inside this present
        CHECK(!gate.Confirmed(true));
        Present(gate, kOff, kOff);
        CHECK(gate.Confirmed(true));
    }

    // A present that ends with the feature active again (re-activation) breaks the run.
    {
        FgOffGate gate;
        Present(gate, kOff, kOff);
        CHECK(gate.Confirmed(true));
        Present(gate, kOff, kActive);
        CHECK(!gate.Confirmed(true));
        CHECK(gate.Count() == 0);
    }

    // A missing feature object is never "off": nothing can be confirmed.
    {
        FgOffGate gate;
        Present(gate, kNone, kNone);
        CHECK(!gate.Confirmed(true));
        Present(gate, kOff, kNone);
        CHECK(!gate.Confirmed(true));
    }

    // Reset (frame generation was asked to go off): earlier presents do not count towards the new request.
    {
        FgOffGate gate;
        Present(gate, kOff, kOff);
        Present(gate, kOff, kOff);
        CHECK(gate.Confirmed(true));
        gate.Reset();
        CHECK(!gate.Confirmed(true));
        // Reset in the middle of a present: that present's entry state is forgotten as well.
        gate.OnPresentBegin(kOff);
        gate.Reset();
        gate.OnPresentEnd(kOff);
        CHECK(!gate.Confirmed(true));
        Present(gate, kOff, kOff);
        CHECK(gate.Confirmed(true));
    }

    // An exit without an entry does not count.
    {
        FgOffGate gate;
        gate.OnPresentEnd(kOff);
        CHECK(!gate.Confirmed(true));
    }

    // FgIsOff and the names.
    CHECK(FgIsOff(kOff) && FgIsOff(kPaused) && !FgIsOff(kActive) && !FgIsOff(kNone));
    CHECK(std::string(FgStateName(kNone)) == "none");
    CHECK(std::string(FgStateName(kActive)) == "active");
    CHECK(std::string(FgStateName(kPaused)) == "paused");
    CHECK(std::string(FgStateName(kOff)) == "off");
    CHECK(std::string(FgStateName(FgSnapshot { true, true, true })) == "active"); // active wins over paused

    return wt::g_failed ? 1 : 0;
}
