// The hotkey step rules of the NR model resolution and the enlargement toggle (ext/nr/NrHotkeys.h).
// One press moves the uniform WorkingScale by exactly 5% of the frame on the 5% grid the Model resolution
// slider offers (25% to 200%), so the hotkey and the slider write bit-identical values; the toggle flips
// Classic and Matched residual and does nothing while the menu combo is greyed out.
#include "ext/nr/NrHotkeys.h"
#include "ext/nr/NrModelSize.h"
#include "ext/nr/NrSettle.h"

#include <cmath>
#include <cstdio>

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

using DlssNr::Hotkeys::StepWorkingScale;
using DlssNr::Hotkeys::ToggleTransfer;
using DlssNr::Hotkeys::TransferToggleable;

// Whole percent of a scale, so float noise never fails an equality check.
int Percent(float scale) { return (int) std::lround(scale * 100.0f); }

void StepSizeAndSign()
{
    // one press is one 5% increment, up and down from 100%
    CHECK(StepWorkingScale(1.0f, true) == 1.05f && Percent(StepWorkingScale(1.0f, true)) == 105);
    CHECK(StepWorkingScale(1.0f, false) == 0.95f && Percent(StepWorkingScale(1.0f, false)) == 95);
    // a halved step (0.025) or a doubled one (0.10) would land off the slider grid
    CHECK(Percent(StepWorkingScale(1.0f, true)) - 100 == 5 && 100 - Percent(StepWorkingScale(1.0f, false)) == 5);
    // each key moves its own direction: up never goes down and down never goes up
    CHECK(StepWorkingScale(0.5f, true) == 0.55f && StepWorkingScale(0.5f, false) == 0.45f);
    CHECK(StepWorkingScale(1.5f, true) == 1.55f && StepWorkingScale(1.5f, false) == 1.45f);
}

void ClampAtBothEnds()
{
    // the range ends hold: pressing past 25% or 200% keeps the end value
    CHECK(StepWorkingScale(0.25f, false) == 0.25f && Percent(StepWorkingScale(0.25f, false)) == 25);
    CHECK(StepWorkingScale(2.0f, true) == 2.0f && Percent(StepWorkingScale(2.0f, true)) == 200);
    // the step into the ends lands on them
    CHECK(StepWorkingScale(0.30f, false) == 0.25f && StepWorkingScale(1.95f, true) == 2.0f);
}

void NoDriftAfterRepeatedPresses()
{
    // ten presses up and ten back return to exactly 100%: the increments never accumulate float error
    float scale = 1.0f;
    for (int i = 0; i < 10; ++i)
        scale = StepWorkingScale(scale, true);
    CHECK(scale == 1.5f && Percent(scale) == 150);
    for (int i = 0; i < 10; ++i)
        scale = StepWorkingScale(scale, false);
    CHECK(scale == 1.0f && Percent(scale) == 100);
    // forty presses each way from the ends cross the whole range and stop dead on the far end
    scale = 0.25f;
    for (int i = 0; i < 40; ++i)
        scale = StepWorkingScale(scale, true);
    CHECK(scale == 2.0f);
    for (int i = 0; i < 40; ++i)
        scale = StepWorkingScale(scale, false);
    CHECK(scale == 0.25f);
    // alternating presses never leave the grid point they started from
    CHECK(StepWorkingScale(StepWorkingScale(0.8f, true), false) == 0.8f);
    CHECK(StepWorkingScale(StepWorkingScale(1.2f, false), true) == 1.2f);
}

void OffGridSnapsInThePressedDirection()
{
    // a value between grid points joins the grid towards the press: 97% goes to 100% on up, 95% on down
    CHECK(StepWorkingScale(0.97f, true) == 1.0f && StepWorkingScale(0.97f, false) == 0.95f);
    CHECK(StepWorkingScale(1.02f, true) == 1.05f && StepWorkingScale(1.02f, false) == 1.0f);
    // a value just below a grid point still steps past it on up (94.9 rounds to the grid point 95, then steps to 100)
    CHECK(StepWorkingScale(0.949f, true) == 1.0f);
}

void BadInputBehavesAs100Percent()
{
    // not-a-number behaves as 100%: up gives 105%, down gives 95%
    CHECK(StepWorkingScale(std::nanf(""), true) == 1.05f && StepWorkingScale(std::nanf(""), false) == 0.95f);
    // zero and negatives are below the range, so they clamp to 25% first and then step from there
    CHECK(StepWorkingScale(0.0f, true) == 0.30f && StepWorkingScale(0.0f, false) == 0.25f);
    CHECK(StepWorkingScale(-1.0f, false) == 0.25f);
    // above the range clamps to 200%: up holds, down steps back onto the grid
    CHECK(StepWorkingScale(3.0f, true) == 2.0f && StepWorkingScale(3.0f, false) == 1.95f);
    // infinity is not a scale either
    CHECK(StepWorkingScale(INFINITY, true) == 1.05f);
}

void ToggleableMatchesTheMenuCombo()
{
    // the toggle works exactly while the model is reduced on some axis and on none above full:
    // the same condition that greys the Enlargement combo
    CHECK(TransferToggleable(0.5f, 0.5f) && TransferToggleable(1.0f, 0.5f) && TransferToggleable(0.5f, 1.0f));
    CHECK(!TransferToggleable(1.0f, 1.0f));                       // at full size the two modes are identical
    CHECK(!TransferToggleable(1.5f, 1.5f) && !TransferToggleable(1.5f, 0.5f)); // above full on any axis
    CHECK(!TransferToggleable(2.0f, 0.25f));                      // the mixed above/below case the pass raises
    CHECK(!TransferToggleable(0.999f, 0.999f) && TransferToggleable(0.998f, 1.0f)); // the menu threshold
}

void ToggleableFromTheKeysTheMenuReads()
{
    // The press handler and the combo both get their scales from EffectiveScales, then ask TransferToggleable.
    using namespace DlssNr;
    const AnamorphicLatch none {};
    auto toggleable = [&](const AnamorphicLatch& latch, float x, float y, float uniform)
    {
        const AxisScales scales = EffectiveScales(latch, x, y, uniform);
        return TransferToggleable(scales.x, scales.y);
    };
    CHECK(toggleable(none, 0.0f, 0.0f, 0.5f));  // the uniform model resolution, reduced
    CHECK(!toggleable(none, 0.0f, 0.0f, 1.0f)); // full size
    CHECK(!toggleable(none, 0.0f, 0.0f, 1.5f)); // enlarged
    CHECK(toggleable(none, 0.5f, 0.0f, 1.0f));  // a per-axis key reduces one axis
    CHECK(!toggleable(none, 1.5f, 0.5f, 1.0f)); // one axis above full: the other is raised, nothing is reduced
    // after a refusal of a per-axis size the pass works at the uniform scale on both axes, and the key follows it
    const AnamorphicLatch refused = LatchAnamorphic(1.5f, 0.5f, 0.5f);
    CHECK(toggleable(refused, 1.5f, 0.5f, 0.5f));
}
void ToggleValuesAndNoOp()
{
    // Classic (0) and Matched residual (1) swap
    CHECK(ToggleTransfer(0u, true) == 1u && ToggleTransfer(1u, true) == 0u);
    CHECK(ToggleTransfer(0u, true) != ToggleTransfer(1u, true));
    // a greyed-out combo leaves the value alone, on either mode
    CHECK(ToggleTransfer(0u, false) == 0u && ToggleTransfer(1u, false) == 1u);
    // a stored value the menu shows as Classic behaves as Classic: it becomes Matched residual
    CHECK(ToggleTransfer(2u, true) == 1u && ToggleTransfer(2u, false) == 2u);
}

void PressesInsideOneSettleRebuildOnce()
{
    // What the pass does with the stepped values: every press rewrites the request while the 500 ms
    // settle has not held, so three quick presses wait three times and rebuild exactly once.
    using DlssNr::Settle::Action;
    using DlssNr::Settle::Decide;
    using DlssNr::Settle::Identity;
    using DlssNr::Settle::kSettleMs;
    DlssNr::Settle::State state {};
    const Identity applied { 3840, 2160, 3840, 2160, 0, 3840, 2160 };
    auto request = applied;
    int rebuilds = 0;
    int64_t now = 1000;
    for (int press = 0; press < 3; ++press)
    {
        // each press steps the uniform scale down 5% and re-derives the working size from it
        const float scale = StepWorkingScale(1.0f - 0.05f * (float) press, false);
        request.workW = (uint32_t) (3840 * scale + 0.5f);
        request.workH = (uint32_t) (2160 * scale + 0.5f);
        const Action action = Decide(state, applied, request, now, false, kSettleMs);
        if (action == Action::Rebuild)
            ++rebuilds;
        else
            CHECK(action == Action::Wait);
        now += 100; // inside the 500 ms settle
    }
    CHECK(rebuilds == 0);
    // the request then holds still past the settle: one rebuild, and the next frame keeps running it
    CHECK(Decide(state, applied, request, now + kSettleMs, false, kSettleMs) == Action::Rebuild);
    CHECK(Decide(state, request, request, now + kSettleMs + 1, false, kSettleMs) == Action::Keep);
}
} // namespace

int main()
{
    StepSizeAndSign();
    ClampAtBothEnds();
    NoDriftAfterRepeatedPresses();
    OffGridSnapsInThePressedDirection();
    BadInputBehavesAs100Percent();
    ToggleableMatchesTheMenuCombo();
    ToggleableFromTheKeysTheMenuReads();
    ToggleValuesAndNoOp();
    PressesInsideOneSettleRebuildOnce();
    if (failures != 0)
    {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("PASS: NR hotkey step and toggle rules (5% grid steps without drift, clamps, off-grid snap, toggle no-op)");
    return 0;
}
