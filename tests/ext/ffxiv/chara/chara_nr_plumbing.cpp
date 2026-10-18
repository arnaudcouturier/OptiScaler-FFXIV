#include "check.h"

#include "ext/ffxiv/chara/CharaNrPlumbing.h"
#include "ext/ffxiv/chara/CharaRoute.h"

using namespace ext::ffxiv::chara;
using namespace ext::ffxiv::chara::nr;

namespace
{
PlumbingFacts Facts(bool sr, bool nr)
{
    PlumbingFacts f;
    f.srWanted = sr;
    f.nrWanted = nr;
    f.srGateOpen = true;
    return f;
}

void TestIndependence()
{
    // both wanted, nothing failed
    PlumbingFacts f = Facts(true, true);
    CHECK(SrUsable(f) && NrUsable(f) && PlumbingActive(f));

    // an SR latch does not stop NR, and the plumbing stays alive for it
    f = Facts(true, true);
    f.srLatched = true;
    CHECK(!SrUsable(f) && NrUsable(f) && PlumbingActive(f));

    // an NR latch does not stop SR
    f = Facts(true, true);
    f.nrLatched = true;
    CHECK(SrUsable(f) && !NrUsable(f) && PlumbingActive(f));

    // a closed SR gate (NGX not ready, different device) does not stop NR
    f = Facts(true, true);
    f.srGateOpen = false;
    CHECK(!SrUsable(f) && NrUsable(f) && PlumbingActive(f));

    // NR alone with PreviewDLSS off
    f = Facts(false, true);
    f.srGateOpen = false;
    CHECK(!SrUsable(f) && NrUsable(f) && PlumbingActive(f));

    // both latched, nothing held: the plumbing is not alive
    f = Facts(true, true);
    f.srLatched = f.nrLatched = true;
    CHECK(!PlumbingActive(f));

    // SR alone behaves as before: a latch ends it, a closed gate makes it inactive
    f = Facts(true, false);
    CHECK(PlumbingActive(f));
    f.srLatched = true;
    CHECK(!PlumbingActive(f));
    f = Facts(true, false);
    f.srGateOpen = false;
    CHECK(!PlumbingActive(f));

    // nothing wanted
    CHECK(!PlumbingActive(Facts(false, false)));

    // the real latch object feeds the same rules the production code uses
    Latches l;
    f = Facts(true, true);
    f.srLatched = l.SrLatched();
    f.nrLatched = l.NrLatched();
    CHECK(SrUsable(f) && NrUsable(f) && PlumbingActive(f));
    l.LatchSr();
    f.srLatched = l.SrLatched();
    f.nrLatched = l.NrLatched();
    CHECK(!SrUsable(f) && NrUsable(f) && PlumbingActive(f));
    l.LatchNr();
    f.srLatched = l.SrLatched();
    f.nrLatched = l.NrLatched();
    CHECK(!SrUsable(f) && !NrUsable(f) && !PlumbingActive(f));
}

void TestHolding()
{
    // a latched or switched-off path stays alive while objects remain, so maintenance can release them
    PlumbingFacts f = Facts(true, false);
    f.srLatched = true;
    f.srSlotsHeld = 2;
    CHECK(PlumbingActive(f) && SrHooksNeeded(f));
    f.srSlotsHeld = 0;
    CHECK(!PlumbingActive(f) && !SrHooksNeeded(f));

    f = Facts(false, false);
    f.nrHoldsResources = true; // PreviewNR was turned off while models exist
    CHECK(PlumbingActive(f));
    CHECK(!NrUsable(f));
    f.nrHoldsResources = false;
    CHECK(!PlumbingActive(f));

    // SR hooks follow SR only; NR alone does not need them
    f = Facts(false, true);
    CHECK(!SrHooksNeeded(f));
    f = Facts(true, true);
    CHECK(SrHooksNeeded(f));
    f.srGateOpen = false;
    CHECK(!SrHooksNeeded(f));
}

void TestDispatcherAndMaintenance()
{
    CHECK(!DispatcherWanted(false, false, false, false, false, false));
    CHECK(DispatcherWanted(false, false, true, false, false, false));
    // PreviewNR switched off but a model still exists: the gate stays open until it is freed
    CHECK(DispatcherWanted(false, false, false, true, false, false));
    CHECK(DispatcherWanted(true, false, false, false, false, false));
    CHECK(DispatcherWanted(false, true, false, false, false, false));
    CHECK(DispatcherWanted(false, false, false, false, false, true));
    // PreviewDLSS switched off while preview slots still hold GPU objects: the gate stays open so the cleanup can run
    CHECK(DispatcherWanted(false, false, false, false, true, false));

    // maintenance: NR alone is maintained and gets the SR cleanup too (it releases nothing it does not hold)
    MaintenancePlan p = PlanMaintenance(false, true, true, false, false);
    CHECK(p.nr && p.sr);
    // PreviewNR off while resources exist: NR is still maintained
    p = PlanMaintenance(false, false, true, true, false);
    CHECK(p.nr && !p.sr);
    // nothing wanted and nothing held: no calls
    p = PlanMaintenance(false, false, false, false, false);
    CHECK(!p.nr && !p.sr);
    // SR alone as before
    p = PlanMaintenance(true, false, true, false, false);
    CHECK(p.sr && !p.nr);
    p = PlanMaintenance(true, false, false, false, false);
    CHECK(!p.sr && !p.nr);
    // NR wanted but the plumbing is not alive (latched): NR is still maintained so it can free what it holds
    p = PlanMaintenance(false, true, false, true, false);
    CHECK(p.nr && !p.sr);

    // PreviewDLSS and PreviewNR are both off but preview slots still hold images and NGX features: the SR cleanup and frame end still
    // run (that is how the slots are released after the option was switched off), whether or not the plumbing says it is alive
    p = PlanMaintenance(false, false, true, false, true);
    CHECK(p.sr && !p.nr);
    p = PlanMaintenance(false, false, false, false, true);
    CHECK(p.sr && !p.nr);
    // a latched SR path with slots held is drained the same way
    p = PlanMaintenance(true, false, false, false, true);
    CHECK(p.sr);
}

// The query bookkeeping of the motion replay is only fed while the SR hooks are forwarded: whenever that changes (either way) what was
// open is forgotten.
void TestEdgeTracker()
{
    EdgeTracker e;
    CHECK(!e.Changed(false));  // starts off: no edge
    CHECK(e.Changed(true));    // off -> on
    CHECK(!e.Changed(true));
    CHECK(!e.Changed(true));
    CHECK(e.Changed(false));   // on -> off
    CHECK(!e.Changed(false));
    CHECK(e.Changed(true));    // the next time it is switched on, again an edge (the old design missed it when nothing asked in between)
}
void TestUnavailable()
{
    // NR wanted but the module cannot run (bridge not up, no NVIDIA card, files missing): it does not claim the draws, so the AA
    // that works without NR keeps working, and the plumbing is not kept alive for it
    PlumbingFacts f = Facts(false, true);
    f.nrAvailable = false;
    CHECK(!NrUsable(f) && !PlumbingActive(f));
    const DrawApi api = DrawApi::DrawIndexed;
    CHECK(ChooseRoute(false, PlumbingActive(f), true, RoleEquipDisplay, api, true) == Route::Enhance);
    // with SR usable the plumbing stays alive for SR
    f = Facts(true, true);
    f.nrAvailable = false;
    CHECK(SrUsable(f) && !NrUsable(f) && PlumbingActive(f));

    // Leftover objects keep the plumbing alive while NR cannot run (NR latched or unavailable but its models are not released yet; the
    // SR slots of a PreviewDLSS that was just switched off): the route must not claim the draw for NR. The NR branch
    // of the route reads "NR usable", never the bare key or "plumbing alive".
    f = Facts(false, true);
    f.nrAvailable = false;
    f.nrHoldsResources = true;
    CHECK(PlumbingActive(f) && !NrUsable(f));
    CHECK(ChooseRoute(false, PlumbingActive(f), true, RoleEquipDisplay, api, NrUsable(f)) == Route::Enhance);
    CHECK(ChooseRoute(false, PlumbingActive(f), false, RoleEquipDisplay, api, NrUsable(f)) == Route::Native);
    f = Facts(false, true);
    f.nrLatched = true;
    f.srSlotsHeld = 2;
    CHECK(PlumbingActive(f) && !NrUsable(f) && !PathCanRun(f));
    CHECK(ChooseRoute(false, PlumbingActive(f), true, RoleEquipDisplay, api, NrUsable(f)) == Route::Enhance);
    // and the bare key would have taken the draw away from the AA
    CHECK(ChooseRoute(false, PlumbingActive(f), true, RoleEquipDisplay, api, f.nrWanted) == Route::Dlss);
}

// The route was Dlss and the module left the draw alone: the AA takes the equipment display draw when PreviewDLSS is off or its
// super-resolution path cannot run. Arguments: (route, handled, dlssWanted, srUsable, aaWanted, roles).
void TestFallBackToAa()
{
    const uint32_t equip = RoleEquipDisplay;
    CHECK(FallBackToAa(Route::Dlss, false, false, false, true, equip));      // declined, AA on, DLSS off, equipment draw: the AA
    CHECK(!FallBackToAa(Route::Dlss, true, false, false, true, equip));      // the plumbing ran the draw
    CHECK(!FallBackToAa(Route::Dlss, false, true, true, true, equip));       // PreviewDLSS on and its path runs: the equipment draw stays native
    CHECK(FallBackToAa(Route::Dlss, false, true, false, true, equip));       // PreviewDLSS on but its path cannot run (NR carried the route): the AA
    CHECK(!FallBackToAa(Route::Dlss, false, false, false, false, equip));    // AA off
    CHECK(!FallBackToAa(Route::Dlss, false, true, false, false, equip));     // AA off, SR path not running: native
    CHECK(!FallBackToAa(Route::Dlss, false, false, false, true, RoleUi));    // not the equipment display draw
    CHECK(!FallBackToAa(Route::Dlss, false, false, false, true, RoleNone));
    CHECK(!FallBackToAa(Route::Native, false, false, false, true, equip));   // other routes: the draw was not offered to the plumbing
    CHECK(!FallBackToAa(Route::Enhance, false, false, false, true, equip));  // the AA already had its turn
    CHECK(FallBackToAa(Route::Dlss, false, false, false, true, equip | RoleUi));
}

// The hook forwarders' gate gives the same answer as SrHooksNeeded(facts) but reads the dear inputs only when they matter.
void TestSrHooksNeededLazy()
{
    for (int bits = 0; bits < 16; ++bits)
    {
        PlumbingFacts f;
        f.srWanted = (bits & 1) != 0;
        f.srLatched = (bits & 2) != 0;
        f.srGateOpen = (bits & 4) != 0;
        f.srSlotsHeld = (bits & 8) != 0 ? 3u : 0u;
        int latchedReads = 0, gateReads = 0;
        const bool lazy = SrHooksNeededLazy(
            f.srSlotsHeld, f.srWanted, [&] { ++latchedReads; return f.srLatched; }, [&] { ++gateReads; return f.srGateOpen; });
        CHECK(lazy == SrHooksNeeded(f));
        if (f.srSlotsHeld != 0 || !f.srWanted)
            CHECK(latchedReads == 0 && gateReads == 0); // the default configuration (PreviewDLSS off, nothing held) reads neither
        if (f.srLatched)
            CHECK(gateReads == 0); // a latched path does not look at the gate
    }
}

void TestConsumerSeen()
{
    ConsumerSeen seen;
    CHECK(!seen.Recent(0x10, 1000, 1000));
    seen.Record(0x10, 1000);
    CHECK(seen.Recent(0x10, 1000, 1000) && seen.Recent(0x10, 2000, 1000)); // inside the window (inclusive)
    CHECK(!seen.Recent(0x10, 2001, 1000));                                  // too old
    CHECK(!seen.Recent(0x20, 1000, 1000));                                  // another image
    CHECK(!seen.Recent(0x10, 900, 1000));                                   // the clock went back: not recent
    seen.Record(0x10, 2500);                                                // refreshed
    CHECK(seen.Recent(0x10, 3400, 1000));
    seen.Record(0, 3000);                                                   // key 0 is never recorded
    CHECK(!seen.Recent(0, 3000, 1000));
    // more images than entries: the one seen longest ago is replaced, recent ones survive
    for (uintptr_t k = 1; k <= ConsumerSeen::kCapacity; ++k)
        seen.Record(0x100 + k, 10000 + k);
    seen.Record(0x999, 20000); // replaces 0x101 (oldest)
    CHECK(seen.Recent(0x999, 20000, 1000) && !seen.Recent(0x101, 10001, 1000));
    CHECK(seen.Recent(0x102, 10002, 1000) && seen.Recent(0x110, 10016, 1000));
}

void TestRoute()
{
    const DrawApi api = DrawApi::DrawIndexed;
    // PreviewNR alone offers the draws to the plumbing when it is alive
    CHECK(ChooseRoute(false, true, false, RoleUi, api, true) == Route::Dlss);
    CHECK(ChooseRoute(false, true, true, RoleEquipDisplay, api, true) == Route::Dlss); // NR before AA
    // NR not usable: the AA works as before, otherwise native
    CHECK(ChooseRoute(false, false, true, RoleEquipDisplay, api, true) == Route::Enhance);
    CHECK(ChooseRoute(false, false, false, RoleUi, api, true) == Route::Native);
    // NR off: unchanged behaviour
    CHECK(ChooseRoute(false, true, false, RoleUi, api, false) == Route::Native);
    CHECK(ChooseRoute(false, true, true, RoleEquipDisplay, api) == Route::Enhance);
    // PreviewDLSS wanted: never the AA while a path can run, with or without NR; when none can run the AA takes the equipment draw
    CHECK(ChooseRoute(true, false, true, RoleEquipDisplay, api, true) == Route::Enhance);
    CHECK(ChooseRoute(true, true, true, RoleEquipDisplay, api, true) == Route::Dlss);
    CHECK(ChooseRoute(true, true, false, RoleNone, api) == Route::Dlss);
}

// PreviewDLSS on but the SR path cannot run: the draw goes to the spatial AA (when on), also while the plumbing is only alive because
// slots of the latched path are still draining. Routing on "plumbing alive" would hand that draw to a path that leaves it native.
void TestRouteFallbackToAa()
{
    const DrawApi api = DrawApi::DrawIndexed;
    PlumbingFacts f = Facts(true, false);
    CHECK(PathCanRun(f));
    CHECK(ChooseRoute(true, PathCanRun(f), true, RoleEquipDisplay, api, false) == Route::Dlss);

    f.srLatched = true; // the path latched after a failure; two slots still drain
    f.srSlotsHeld = 2;
    CHECK(PlumbingActive(f)); // alive: the cleanup must keep running
    CHECK(!PathCanRun(f));    // but no draw can be handled
    CHECK(ChooseRoute(true, PathCanRun(f), true, RoleEquipDisplay, api, false) == Route::Enhance);
    CHECK(ChooseRoute(true, PathCanRun(f), false, RoleEquipDisplay, api, false) == Route::Native);  // AA off: native

    f = Facts(true, false);
    f.srGateOpen = false; // NGX not initialised yet
    CHECK(!PathCanRun(f));
    CHECK(ChooseRoute(true, PathCanRun(f), true, RoleEquipDisplay, api, false) == Route::Enhance);

    // NR can run while SR cannot: the plumbing takes the draw
    f = Facts(true, true);
    f.srLatched = true;
    CHECK(PathCanRun(f));
    CHECK(ChooseRoute(true, PathCanRun(f), true, RoleEquipDisplay, api, true) == Route::Dlss);
    f.nrLatched = true;
    CHECK(!PathCanRun(f));
    CHECK(ChooseRoute(true, PathCanRun(f), true, RoleEquipDisplay, api, true) == Route::Enhance);
}

// The truth table of the bring-up gate: the SR path in charge of a preview (usable, slot not failed, preview not too large for it) means no
// bring-up for any render of that preview; every other combination keeps the bring-up. What the user sees: with both options on, a render the
// SR path did not evaluate keeps the native image (no throwaway 1x NR model at window open, no bring-up/production flapping).
void TestBringUpGate()
{
    for (int bits = 0; bits < 8; ++bits)
    {
        const bool srUsable = (bits & 1) != 0, slotFailed = (bits & 2) != 0, tooLarge = (bits & 4) != 0;
        const bool inCharge = SrInCharge(srUsable, slotFailed, tooLarge);
        CHECK(inCharge == (srUsable && !slotFailed && !tooLarge));
        PlumbingFacts f = Facts(true, true);
        CHECK(RunBringUpNow(f, true, true, inCharge) == !inCharge);
        // the other conditions still apply
        CHECK(!RunBringUpNow(f, false, true, inCharge) && !RunBringUpNow(f, true, false, inCharge));
        f.nrLatched = true;
        CHECK(!RunBringUpNow(f, true, true, inCharge));
    }
    // PreviewDLSS off: the SR path is not usable, the bring-up runs for a shown window
    const PlumbingFacts nrOnly = Facts(false, true);
    CHECK(RunBringUpNow(nrOnly, true, true, SrInCharge(SrUsable(nrOnly), false, false)));
    // both on, the SR path in charge: no bring-up
    const PlumbingFacts both = Facts(true, true);
    CHECK(SrUsable(both) && !RunBringUpNow(both, true, true, SrInCharge(SrUsable(both), false, false)));
    // the slot failed, or the preview is too large for SR: the bring-up takes over
    CHECK(RunBringUpNow(both, true, true, SrInCharge(true, true, false)));
    CHECK(RunBringUpNow(both, true, true, SrInCharge(true, false, true)));
    // the SR path latched or its gate closed: the bring-up takes over
    PlumbingFacts latched = both;
    latched.srLatched = true;
    CHECK(RunBringUpNow(latched, true, true, SrInCharge(SrUsable(latched), false, false)));
}

// The cyan tint targets and the helper passes keep the dispatcher's gate and maintenance on until they are released, also with every key off
// (before, only the slots counted, so a tint-only attempt left up to 16 RGBA16F targets and the helper passes allocated until the hooks went).
void TestTintAndPassesKeepTheGateOpen()
{
    for (int bits = 0; bits < 8; ++bits)
    {
        const uint32_t slots = (bits & 1) ? 2 : 0, tint = (bits & 2) ? 3 : 0;
        const bool passes = (bits & 4) != 0;
        const bool holds = PlumbingHoldsObjects(slots, tint, passes);
        CHECK(holds == (bits != 0));
        // every key off: the gate and the SR maintenance follow what is held
        CHECK(DispatcherWanted(false, false, false, false, holds, false) == holds);
        CHECK(PlanMaintenance(false, false, false, false, holds).sr == holds);
    }

    // the release rule
    CHECK(ReleaseHelperPasses(true, 0, 0, true));
    CHECK(!ReleaseHelperPasses(false, 0, 0, true)); // the SR path is enabled: the passes stay
    CHECK(!ReleaseHelperPasses(true, 1, 0, true));  // a slot still uses them
    CHECK(!ReleaseHelperPasses(true, 0, 2, true));  // tint targets still exist
    CHECK(!ReleaseHelperPasses(true, 0, 0, false)); // nothing to release

    // a tint-only run (PreviewNR and the tint on, PreviewDLSS off, then PreviewNR off): the gate stays open until the last tint target is
    // released and then the passes go with it
    uint32_t tint = 2;
    bool passes = true;
    CHECK(DispatcherWanted(false, false, false, false, PlumbingHoldsObjects(0, tint, passes), false)); // the key is off, the gate is open
    if (ReleaseHelperPasses(true, 0, tint, passes))
        passes = false;
    CHECK(passes);       // tint targets still exist
    tint = 0;            // aged out (kNrTintUnusedMs)
    CHECK(DispatcherWanted(false, false, false, false, PlumbingHoldsObjects(0, tint, passes), false)); // the passes keep the gate open one more pass
    if (ReleaseHelperPasses(true, 0, tint, passes))
        passes = false;
    CHECK(!passes);
    CHECK(!DispatcherWanted(false, false, false, false, PlumbingHoldsObjects(0, tint, passes), false)); // everything released: the gate closes
}

void TestHooks()
{
    PlumbingFacts f = Facts(true, true);
    // NR bring-up needs only an accepted finisher and a usable NR; SR state plays no part
    CHECK(RunBringUpNow(f, true, true, false));
    CHECK(!RunBringUpNow(f, false, true, false));
    CHECK(!RunBringUpNow(f, true, false, false)); // nobody shows this preview: no model
    // The fourth argument is "the SR path is in charge of this preview" (SrInCharge): then the bring-up is never run, whether or not SR
    // evaluated this render; the render keeps the native image like PreviewDLSS alone, and NR works on the SR result (production).
    CHECK(!RunBringUpNow(f, true, true, true));
    // ... while a preview the SR path does not serve (PreviewDLSS off, SR latched or unusable, a failed or too large slot) gets the bring-up
    CHECK(RunBringUpNow(f, true, true, false));
    f.srLatched = true;
    CHECK(RunBringUpNow(f, true, true, false));
    f.srGateOpen = false;
    CHECK(RunBringUpNow(f, true, true, false));
    f.nrLatched = true;
    CHECK(!RunBringUpNow(f, true, true, false));
    f = Facts(false, false);
    CHECK(!RunBringUpNow(f, true, true, false));
    f = Facts(false, true);
    f.nrAvailable = false; // bridge not up, no NVIDIA card or files missing
    CHECK(!RunBringUpNow(f, true, true, false));

    // the production entry needs an SR evaluate that succeeded for this render, a window somebody shows, and a usable NR
    f = Facts(true, true);
    CHECK(RunProductionNow(f, true, true));
    CHECK(!RunProductionNow(f, false, true));  // the SR evaluate failed or did not run: there is no SR output to improve
    CHECK(!RunProductionNow(f, true, false));  // nobody shows this preview
    f.nrLatched = true;
    CHECK(!RunProductionNow(f, true, true));
    f = Facts(true, false);
    CHECK(!RunProductionNow(f, true, true));   // PreviewNR off
    f = Facts(true, true);
    f.nrAvailable = false;
    CHECK(!RunProductionNow(f, true, true));   // the module cannot run (bridge, card, files)

    // A finisher without an NR result withdraws the older result, but only while the module holds something (nothing to withdraw
    // otherwise, and the call is skipped on the hot path)
    f = Facts(false, true);
    f.nrHoldsResources = true;
    CHECK(InvalidateNow(f, false));   // rejected finisher, bring-up not run, skipped or failed
    CHECK(!InvalidateNow(f, true));   // NR produced the result for this render
    f.nrHoldsResources = false;
    CHECK(!InvalidateNow(f, false));  // nothing exists
    f = Facts(false, false);
    f.nrHoldsResources = true;        // PreviewNR switched off while models are still released
    CHECK(InvalidateNow(f, false));

    // consumer priority: the flood beats everything, then production > SR > bring-up NR > native
    CHECK(ChooseConsumerSource(true, true, true, true) == ConsumerSource::Flood);
    CHECK(ChooseConsumerSource(true, false, false, false) == ConsumerSource::Flood);
    CHECK(ChooseConsumerSource(false, true, true, true) == ConsumerSource::ProductionResult);  // production over SR over bring-up
    CHECK(ChooseConsumerSource(false, true, true, false) == ConsumerSource::ProductionResult);
    CHECK(ChooseConsumerSource(false, false, true, true) == ConsumerSource::SrResult);          // SR over bring-up
    CHECK(ChooseConsumerSource(false, false, true, false) == ConsumerSource::SrResult);
    CHECK(ChooseConsumerSource(false, false, false, true) == ConsumerSource::NrResult);
    CHECK(ChooseConsumerSource(false, false, false, false) == ConsumerSource::Native);
    // the production result is the SR slot's composed image, so it is never reported without an SR result being ready ... but if a caller
    // passes it alone, production still wins over a stale bring-up view
    CHECK(ChooseConsumerSource(false, true, false, true) == ConsumerSource::ProductionResult);
    for (int bits = 0; bits < 16; ++bits)
    {
        const bool flood = (bits & 1) != 0, prod = (bits & 2) != 0, sr = (bits & 4) != 0, nr = (bits & 8) != 0;
        const ConsumerSource expected = flood  ? ConsumerSource::Flood
                                        : prod ? ConsumerSource::ProductionResult
                                        : sr   ? ConsumerSource::SrResult
                                        : nr   ? ConsumerSource::NrResult
                                               : ConsumerSource::Native;
        CHECK(ChooseConsumerSource(flood, prod, sr, nr) == expected);
    }

    // The alpha pass takes the NR output only when production produced one, and retries with the SR output when it fails with the NR one
    CHECK(ChooseAlphaInput(true) == AlphaInput::NrOutput && ChooseAlphaInput(false) == AlphaInput::SrOutput);
    CHECK(RetryAlphaWithSr(AlphaInput::NrOutput, false));
    CHECK(!RetryAlphaWithSr(AlphaInput::NrOutput, true));
    CHECK(!RetryAlphaWithSr(AlphaInput::SrOutput, false)); // a failure with the SR output is a failure of the SR path, no retry
    CHECK(!RetryAlphaWithSr(AlphaInput::SrOutput, true));

    // A render whose production NR ran counts as "NR produced a result": the dispatcher must not withdraw it in the same render
    f = Facts(true, true);
    f.nrHoldsResources = true;
    CHECK(InvalidateNow(f, NrProducedForRender(false, false)));
    CHECK(!InvalidateNow(f, NrProducedForRender(false, true)));  // production ran
    CHECK(!InvalidateNow(f, NrProducedForRender(true, false)));  // bring-up ran
    CHECK(!InvalidateNow(f, NrProducedForRender(true, true)));

    // the debug tint: magenta for SR, cyan for anything NR produced, nothing when off
    CHECK(AlphaTintFor(false, AlphaInput::SrOutput) == TintKind::None && AlphaTintFor(false, AlphaInput::NrOutput) == TintKind::None);
    CHECK(AlphaTintFor(true, AlphaInput::SrOutput) == TintKind::Sr);
    CHECK(AlphaTintFor(true, AlphaInput::NrOutput) == TintKind::Nr);
    // the bring-up result gets its own tint pass: once per result, only for the NR result source, only with the tint on
    CHECK(ResultTintPassDue(true, ConsumerSource::NrResult, 0, 7));    // never tinted
    CHECK(ResultTintPassDue(true, ConsumerSource::NrResult, 6, 7));    // a newer result
    CHECK(!ResultTintPassDue(true, ConsumerSource::NrResult, 7, 7));   // the target already shows this result
    CHECK(!ResultTintPassDue(false, ConsumerSource::NrResult, 0, 7));  // tint off
    CHECK(!ResultTintPassDue(true, ConsumerSource::SrResult, 0, 7));   // the SR image is tinted by its alpha pass
    CHECK(!ResultTintPassDue(true, ConsumerSource::ProductionResult, 0, 7));
    CHECK(!ResultTintPassDue(true, ConsumerSource::Native, 0, 7));

    // log once per cause
    CauseLog causes;
    CHECK(causes.First(1) && !causes.First(1) && causes.First(2) && !causes.First(2) && !causes.First(1));
    CHECK(!causes.First(CauseLog::kCauses)); // out of range: never

    // The NR result is asked for by ONE rule, for an image with an SR slot and for one without (before, the no-slot branch of the consumer
    // repeated it inline and dropping its flood term passed every test): NR may run, no flood, SR result not there, a preview output.
    for (int bits = 0; bits < 16; ++bits)
    {
        const bool nrOn = (bits & 1) != 0, floodOn = (bits & 2) != 0, srReady = (bits & 4) != 0, previewOutput = (bits & 8) != 0;
        CHECK(WantNrResult(nrOn, floodOn, srReady, previewOutput) == (nrOn && !floodOn && !srReady && previewOutput));
    }
    CHECK(WantNrResult(true, false, false, true));
    CHECK(!WantNrResult(true, true, false, true));   // the flood test wins, with or without an SR slot
    CHECK(!WantNrResult(true, false, true, true));   // the SR result wins
    CHECK(!WantNrResult(true, false, false, false)); // not a preview output
    CHECK(!WantNrResult(false, false, false, true)); // NR cannot run
}
} // namespace

int main()
{
    TestIndependence();
    TestHolding();
    TestDispatcherAndMaintenance();
    TestEdgeTracker();
    TestRouteFallbackToAa();
    TestRoute();
    TestUnavailable();
    TestFallBackToAa();
    TestSrHooksNeededLazy();
    TestConsumerSeen();
    TestHooks();
    TestBringUpGate();
    TestTintAndPassesKeepTheGateOpen();
    return TEST_RESULT("chara_nr_plumbing");
}
