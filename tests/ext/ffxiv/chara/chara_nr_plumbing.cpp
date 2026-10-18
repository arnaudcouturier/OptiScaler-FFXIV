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

    // the real latch object backs the same rules
    Latches l;
    CHECK(l.PlumbingAlive(true, true));
    l.LatchSr();
    CHECK(l.NrMayRun(true) && !l.SrMayRun(true) && l.PlumbingAlive(true, true) && l.PlumbingAlive(false, true));
    l.LatchNr();
    CHECK(!l.PlumbingAlive(true, true));
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
    // SR slots of a PreviewDLSS that was just switched off): the route must not claim the draw for NR (REVIEW-NR-2 M1 a). The NR branch
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

// The route was Dlss and the module left the draw alone: the AA takes the equipment display draw when PreviewDLSS is off (REVIEW-NR-2 M1 b).
void TestFallBackToAa()
{
    const uint32_t equip = RoleEquipDisplay;
    CHECK(FallBackToAa(Route::Dlss, false, false, true, equip));      // declined, AA on, DLSS off, equipment draw: the AA
    CHECK(!FallBackToAa(Route::Dlss, true, false, true, equip));      // the plumbing ran the draw
    CHECK(!FallBackToAa(Route::Dlss, false, true, true, equip));      // PreviewDLSS on: the equipment draw stays native (specification)
    CHECK(!FallBackToAa(Route::Dlss, false, false, false, equip));    // AA off
    CHECK(!FallBackToAa(Route::Dlss, false, false, true, RoleUi));    // not the equipment display draw
    CHECK(!FallBackToAa(Route::Dlss, false, false, true, RoleNone));
    CHECK(!FallBackToAa(Route::Native, false, false, true, equip));   // other routes: the draw was not offered to the plumbing
    CHECK(!FallBackToAa(Route::Enhance, false, false, true, equip));  // the AA already had its turn
    CHECK(FallBackToAa(Route::Dlss, false, false, true, equip | RoleUi));
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

void TestHooks()
{
    PlumbingFacts f = Facts(true, true);
    // NR bring-up needs only an accepted finisher and a usable NR; SR state plays no part
    CHECK(RunBringUpNow(f, true, true, false));
    CHECK(!RunBringUpNow(f, false, true, false));
    CHECK(!RunBringUpNow(f, true, false, false)); // nobody shows this preview: no model
    // PreviewDLSS and PreviewNR both on: a render that SR evaluated shows the SR result, so NR is not run for it (and costs nothing) ...
    CHECK(!RunBringUpNow(f, true, true, true));
    // ... while a render SR did not evaluate (waiting for guides, failed, latched) still gets the NR result
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

    // the production entry needs an SR evaluate that succeeded
    f = Facts(true, true);
    CHECK(RunProductionNow(f, true));
    CHECK(!RunProductionNow(f, false));
    f.nrLatched = true;
    CHECK(!RunProductionNow(f, true));
    f = Facts(true, false);
    CHECK(!RunProductionNow(f, true));

    // consumer priority: the flood beats everything, SR beats NR
    CHECK(ChooseConsumerSource(true, true, true) == ConsumerSource::Flood);
    CHECK(ChooseConsumerSource(false, true, true) == ConsumerSource::SrResult);
    CHECK(ChooseConsumerSource(false, false, true) == ConsumerSource::NrResult);
    CHECK(ChooseConsumerSource(false, true, false) == ConsumerSource::SrResult);
    CHECK(ChooseConsumerSource(false, false, false) == ConsumerSource::Native);
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
    TestConsumerSeen();
    TestHooks();
    return TEST_RESULT("chara_nr_plumbing");
}
