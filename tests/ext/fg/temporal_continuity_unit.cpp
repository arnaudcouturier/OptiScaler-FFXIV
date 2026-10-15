// C5/B5: SuccessfulFrames (ext/fg/TemporalContinuity.h), the history rule behind the DLSS-G and XeFG reset decisions.
// The reference build's dlssg_delayed_guides test sliced IFGFeature_Dx12's text to exercise the guide matcher; the
// matcher (ext/fg/GuideMatcher.cpp) is only reachable through a live IFGFeature_Dx12, so it is not ported (PORT-LOG S5).
#include "ext/fg/TemporalContinuity.h"

#include <cassert>
#include <cstdio>

int main()
{
    ext::fg::SuccessfulFrames h;
    assert(h.NeedsReset(10)); // no history yet
    h.Commit(10);
    assert(!h.NeedsReset(11)); // next consecutive frame keeps the history
    assert(h.NeedsReset(12));  // a skipped frame is a gap
    assert(h.NeedsReset(10));  // repeat / going back
    assert(h.NeedsReset(9));
    h.Commit(11);
    assert(!h.NeedsReset(12));
    h.Invalidate();
    assert(h.NeedsReset(12)); // deactivation drops the history
    h.Commit(30);
    assert(h.NeedsReset(33) && !h.NeedsReset(31));
    std::puts("SuccessfulFrames: warm-up, consecutive, gap, repeat, invalidate passed");
    return 0;
}
