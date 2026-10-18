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
    CHECK(!DispatcherWanted(false, false, false, false, false));
    CHECK(DispatcherWanted(false, false, true, false, false));
    // PreviewNR switched off but a model still exists: the gate stays open until it is freed
    CHECK(DispatcherWanted(false, false, false, true, false));
    CHECK(DispatcherWanted(true, false, false, false, false));
    CHECK(DispatcherWanted(false, true, false, false, false));
    CHECK(DispatcherWanted(false, false, false, false, true));

    // maintenance: NR alone is maintained and gets the SR cleanup too (it releases nothing it does not hold)
    MaintenancePlan p = PlanMaintenance(false, true, true, false);
    CHECK(p.nr && p.sr);
    // PreviewNR off while resources exist: NR is still maintained
    p = PlanMaintenance(false, false, true, true);
    CHECK(p.nr && !p.sr);
    // nothing wanted and nothing held: no calls
    p = PlanMaintenance(false, false, false, false);
    CHECK(!p.nr && !p.sr);
    // SR alone as before
    p = PlanMaintenance(true, false, true, false);
    CHECK(p.sr && !p.nr);
    p = PlanMaintenance(true, false, false, false);
    CHECK(!p.sr && !p.nr);
    // NR wanted but the plumbing is not alive (latched): NR is still maintained so it can free what it holds
    p = PlanMaintenance(false, true, false, true);
    CHECK(p.nr && !p.sr);
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
    // PreviewDLSS wanted: never the AA, with or without NR
    CHECK(ChooseRoute(true, false, true, RoleEquipDisplay, api, true) == Route::Native);
    CHECK(ChooseRoute(true, true, true, RoleEquipDisplay, api, true) == Route::Dlss);
    CHECK(ChooseRoute(true, true, false, RoleNone, api) == Route::Dlss);
}

void TestHooks()
{
    PlumbingFacts f = Facts(true, true);
    // NR bring-up needs only an accepted finisher and a usable NR; SR state plays no part
    CHECK(RunBringUpNow(f, true));
    CHECK(!RunBringUpNow(f, false));
    f.srLatched = true;
    CHECK(RunBringUpNow(f, true));
    f.srGateOpen = false;
    CHECK(RunBringUpNow(f, true));
    f.nrLatched = true;
    CHECK(!RunBringUpNow(f, true));
    f = Facts(false, false);
    CHECK(!RunBringUpNow(f, true));

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
    TestRoute();
    TestHooks();
    return TEST_RESULT("chara_nr_plumbing");
}
