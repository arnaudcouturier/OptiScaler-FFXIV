// Review N1: the preview NR module must notice a changed NR runtime selection even while the gate that the draw hook tests is CLOSED
// (a model failure latch, or "no model file"). This drives the production observer (ext/nr/runtime/NrPreviewRuntime.h) through the real
// availability rules (Evaluate in CharaNrRules.h) and the real draw gate (NrUsable in CharaNrPlumbing.h), with a small model of the module
// whose draw hook and maintenance call mirror CharaNr.cpp. The "old wiring" runs show the stuck state, so the test cannot pass by
// accident of how it is set up.
#include "check.h"

#include "ext/ffxiv/chara/CharaNrPlumbing.h"
#include "ext/ffxiv/chara/CharaNrRules.h"
#include "ext/nr/runtime/NrPreviewRuntime.h"

#include <map>

using namespace ext::ffxiv::chara::nr;
using ext::nr::runtime::ObservePreviewRuntime;
using ext::nr::runtime::PreviewRuntimeActions;
using ext::nr::runtime::PreviewRuntimeView;

namespace
{
struct Module
{
    // wiring switches: the first version only looked at the revision inside the draw entry (behind the gate)
    bool observeInMaintain = true;

    std::map<uint64_t, bool> installed; // runtime revision -> a model file exists for it
    uint64_t current = 1;               // PreviewRuntimeRevision()

    bool inited = false;
    bool modelLatched = false;
    uint64_t latchedRevision = 0;
    Probe model = Probe::Unknown;
    uint64_t backendRevision = 0;
    uint64_t probedRevision = 0;
    bool available = true; // the cached availability the draw hook reads (nr::Available())
    int runs = 0;          // draws that reached RunCoreSteps and got past the availability

    GateFacts Facts() const
    {
        GateFacts f;
        f.configOn = true;
        f.latched = modelLatched;
        f.bridgeUp = true;
        f.nvidia = Probe::Present;
        f.forwarder = Probe::Present;
        f.model = model;
        return f;
    }
    Availability Refresh()
    {
        const Availability a = Evaluate(Facts());
        available = a == Availability::Available;
        return a;
    }
    void LatchModel()
    {
        modelLatched = true;
        latchedRevision = current;
    }
    void Observe()
    {
        PreviewRuntimeView v;
        v.inited = inited;
        v.modelMissing = model == Probe::Missing;
        v.modelLatched = modelLatched;
        v.backendRevision = backendRevision;
        v.probedRevision = probedRevision;
        v.latchedRevision = latchedRevision;
        const PreviewRuntimeActions a = ObservePreviewRuntime(v, current);
        if (a.clearModelLatch)
            modelLatched = false;
        if (a.resync) // SyncRuntime: AcquirePreviewBackend for the new revision
        {
            backendRevision = current;
            model = installed[current] ? Probe::Present : Probe::Missing;
        }
        if (a.reprobe)
            model = Probe::Unknown; // EnsureInit probes again
    }
    // EnsureInit: the first acquisition of the backend
    bool EnsureInit()
    {
        if (inited)
            return true;
        if (model == Probe::Unknown)
        {
            probedRevision = current;
            model = installed[current] ? Probe::Present : Probe::Missing;
            backendRevision = current;
        }
        if (Refresh() != Availability::Available)
            return false;
        inited = true;
        return true;
    }
    // the draw hook: RunCoreSteps is reached only while preview NR is usable
    bool Draw()
    {
        PlumbingFacts pf;
        pf.nrWanted = true;
        pf.nrLatched = modelLatched;
        pf.nrAvailable = available;
        if (!NrUsable(pf))
            return false; // the gate is closed: nothing below runs
        Observe();        // RunCoreSteps: before the availability is read
        if (Refresh() != Availability::Available || !EnsureInit())
            return false;
        ++runs;
        return true;
    }
    // MaintainImpl: runs regardless of the gate
    void Maintain()
    {
        if (observeInMaintain)
            Observe();
        Refresh();
    }
    void Frames(int n = 3)
    {
        for (int i = 0; i < n; ++i)
        {
            Draw();
            Maintain();
        }
    }
};
} // namespace

static void ModelLatchRecoversThroughTheGate()
{
    for (const bool fixedWiring : { false, true })
    {
        Module m;
        m.observeInMaintain = fixedWiring;
        m.installed = { { 1, true }, { 2, true } };
        m.Frames();
        CHECK(m.runs > 0 && m.inited && m.available);
        m.LatchModel(); // the model of revision 1 failed in a preview
        m.Maintain();
        CHECK(!m.available); // Availability::Latched: the draw hook stops calling in
        const int before = m.runs;
        m.Frames();
        CHECK(m.runs == before); // nothing runs while the latch stands and the selection is the same
        CHECK(m.modelLatched);
        m.current = 2; // the user switches to another model
        m.Frames();
        if (fixedWiring)
        {
            CHECK(!m.modelLatched && m.available && m.runs > before); // recovered through the gate
            CHECK(m.backendRevision == 2);
        }
        else
        {
            CHECK(m.modelLatched && !m.available && m.runs == before); // the old wiring stays stuck: the test goes through the gate
        }
    }
}

static void StartedWithoutAModelThenOneIsSelected()
{
    for (const bool fixedWiring : { false, true })
    {
        Module m;
        m.observeInMaintain = fixedWiring;
        m.installed = { { 1, false }, { 2, true } };
        m.Frames();
        CHECK(!m.inited && m.model == Probe::Missing && !m.available);
        CHECK(m.runs == 0);
        m.current = 2;
        m.Frames();
        if (fixedWiring)
            CHECK(m.inited && m.available && m.runs > 0 && m.model == Probe::Present);
        else
            CHECK(!m.inited && m.runs == 0);
    }
}

static void NotInstalledEntryThenAnInstalledOne()
{
    for (const bool fixedWiring : { false, true })
    {
        Module m;
        m.observeInMaintain = fixedWiring;
        m.installed = { { 1, true }, { 2, false }, { 3, true } };
        m.Frames();
        CHECK(m.inited && m.available);
        m.current = 2; // the user clicks an entry that is not installed
        m.Frames();
        if (fixedWiring)
            CHECK(m.model == Probe::Missing && !m.available); // previews keep the native image
        const int before = m.runs;
        m.current = 3; // and then an installed one
        m.Frames();
        if (fixedWiring)
            CHECK(m.model == Probe::Present && m.available && m.runs > before && m.backendRevision == 3);
        else
            CHECK(m.model == Probe::Missing && !m.available && m.runs == before); // the old wiring stays stuck behind the closed gate
    }
}

static void NothingHappensWithoutAChange()
{
    Module m;
    m.installed = { { 1, true } };
    m.Frames();
    const int runs = m.runs;
    m.Frames(10);
    CHECK(m.runs == runs + 20 || m.runs > runs); // keeps running
    CHECK(m.backendRevision == 1 && !m.modelLatched);
    // the observer itself: no change, no action
    PreviewRuntimeView v;
    v.inited = true;
    v.backendRevision = 5;
    const auto a = ObservePreviewRuntime(v, 5);
    CHECK(!a.clearModelLatch && !a.resync && !a.reprobe);
    // a latch set at revision 5 is not cleared at revision 5, and is at 6 even when the module was already released (not inited)
    PreviewRuntimeView w;
    w.modelLatched = true;
    w.latchedRevision = 5;
    CHECK(!ObservePreviewRuntime(w, 5).clearModelLatch);
    CHECK(ObservePreviewRuntime(w, 6).clearModelLatch);
}

int main()
{
    ModelLatchRecoversThroughTheGate();
    StartedWithoutAModelThenOneIsSelected();
    NotInstalledEntryThenAnInstalledOne();
    NothingHappensWithoutAChange();
    return TEST_RESULT("chara_nr_runtime_gate");
}
