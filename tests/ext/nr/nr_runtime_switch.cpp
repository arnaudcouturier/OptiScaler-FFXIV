// NR runtime switch state machine (ext/nr/runtime/NrRuntimeSwitch.h) against a fake host that models the pass: features are created
// from the current model path, parked on a switch, and the test checks the order of the steps, the recovery after an unavailable
// model, and that 20 rapid switches end in exactly the last selection with no feature left behind.
#include "ext/nr/runtime/NrRuntimeSwitch.h"

#include <cstdio>
#include <set>
#include <string>

using namespace ext::nr::runtime;

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

static Entry Folder(const wchar_t* name)
{
    Entry e;
    e.id = name;
    e.path = std::filesystem::path(L"X:/g/OptiScaler") / name / L"nvngx_dlssnr.dll";
    e.folderVersion = *ParseFolderVersion(name);
    return e;
}

struct FakeHost
{
    std::vector<std::string> steps;       // the order things happened in
    std::filesystem::path model;          // current model path ("" = none)
    FailureState fail;                    // the pass's failure latch (the production class)
    std::set<int> live;                   // features built and not yet parked
    int nextFeature = 1;
    int created = 0, parked = 0;
    bool historyClear = false;
    int reuseDrops = 0;
    std::vector<std::filesystem::path> builtFrom; // model path of each created feature

    void ClearModelFailure()
    {
        steps.push_back("failure");
        ForgetModelFailure(fail);
    }
    // an evaluate that returns an error: the model's fault
    void EvaluateFails() { FailModel(fail, "the model refused to run"); }
    void ParkFeatures()
    {
        steps.push_back("park");
        parked += static_cast<int>(live.size());
        live.clear();
    }
    void ClearHistory()
    {
        steps.push_back("history");
        historyClear = true;
    }
    void DropReuseCaches()
    {
        steps.push_back("reuse");
        ++reuseDrops;
    }
    void UseModel(const Snapshot& s)
    {
        steps.push_back("use");
        model = s.path;
    }
    void MarkUnavailable(const Snapshot& s)
    {
        steps.push_back("unavailable");
        model.clear();
        FailModel(fail, UnavailableReason(s));
    }
    // what the pass does every frame after the switch check
    void Frame()
    {
        if (!fail.failed && !model.empty() && live.empty())
        {
            live.insert(nextFeature++);
            ++created;
            builtFrom.push_back(model);
        }
    }
};

static void OrderAndFirstRevision()
{
    FakeHost host;
    uint64_t applied = 0;
    Snapshot s = MakeSnapshot({ Folder(L"NR.310.9") }, L"");
    s.revision = 1;
    CHECK(ApplyRuntimeChange(host, s, applied) == SwitchResult::Initial);
    CHECK(applied == 1);
    const std::vector<std::string> expected { "failure", "park", "history", "reuse", "use" };
    CHECK(host.steps == expected);
    CHECK(ApplyRuntimeChange(host, s, applied) == SwitchResult::None); // same revision: nothing happens
    CHECK(host.steps == expected);
    host.Frame();
    CHECK(host.created == 1 && host.builtFrom[0] == s.path);
}

static void SwitchParksTheOldFeature()
{
    FakeHost host;
    uint64_t applied = 0;
    Snapshot a = MakeSnapshot({ Folder(L"NR.310.8") }, L"");
    a.revision = 1;
    Snapshot b = MakeSnapshot({ Folder(L"NR.310.9") }, L"");
    b.revision = 2;
    ApplyRuntimeChange(host, a, applied);
    host.Frame();
    CHECK(host.live.size() == 1u);
    host.steps.clear();
    CHECK(ApplyRuntimeChange(host, b, applied) == SwitchResult::Switched);
    const std::vector<std::string> expected { "failure", "park", "history", "reuse", "use" };
    CHECK(host.steps == expected);
    CHECK(host.live.empty() && host.parked == 1);
    CHECK(host.model == b.path);
    host.Frame();
    CHECK(host.created == 2 && host.builtFrom[1] == b.path);
}

static void UnavailableAndRecovery()
{
    FakeHost host;
    uint64_t applied = 0;
    const std::vector<Entry> entries { Folder(L"NR.310.9") };
    Snapshot ok = MakeSnapshot(entries, L"NR.310.9");
    ok.revision = 1;
    Snapshot missing = MakeSnapshot(entries, L"NR.310.8");
    missing.revision = 2;
    Snapshot back = MakeSnapshot(entries, L"NR.310.9");
    back.revision = 3;
    ApplyRuntimeChange(host, ok, applied);
    host.Frame();
    CHECK(ApplyRuntimeChange(host, missing, applied) == SwitchResult::Unavailable);
    CHECK(host.fail.failed && host.fail.kind == FailureKind::Model && host.model.empty());
    CHECK(host.live.empty() && host.parked == 1); // the old model's feature was parked, not left running
    host.Frame();
    CHECK(host.created == 1); // nothing is built while there is no model
    // switching back recovers: the latch caused by the model does not survive a new revision
    CHECK(ApplyRuntimeChange(host, back, applied) == SwitchResult::Switched);
    CHECK(!host.fail.failed && host.model == back.path);
    host.Frame();
    CHECK(host.created == 2);
}

static void OtherFailuresSurvive()
{
    FakeHost host;
    uint64_t applied = 0;
    FailGlobal(host.fail, "nvngx.dll_dlssnr.dll would not load"); // not the model's fault
    Snapshot s = MakeSnapshot({ Folder(L"NR.310.9") }, L"");
    s.revision = 1;
    ApplyRuntimeChange(host, s, applied);
    CHECK(host.fail.failed);
    host.Frame();
    CHECK(host.created == 0);
}

static void EvaluateFailureRecoversOnSwitch()
{
    // review H1: a model that fails at EVALUATE is forgotten by a switch, like one that fails at create
    FakeHost host;
    uint64_t applied = 0;
    Snapshot a = MakeSnapshot({ Folder(L"NR.310.8") }, L"");
    a.revision = 1;
    Snapshot b = MakeSnapshot({ Folder(L"NR.310.9") }, L"");
    b.revision = 2;
    Snapshot backToA = a;
    backToA.revision = 3;
    ApplyRuntimeChange(host, a, applied);
    host.Frame();
    host.EvaluateFails();
    CHECK(host.fail.failed && host.fail.kind == FailureKind::Model);
    host.Frame();
    CHECK(host.created == 1); // nothing is built while the latch is set
    ApplyRuntimeChange(host, b, applied);
    CHECK(!host.fail.failed);
    host.Frame();
    CHECK(host.created == 2 && host.builtFrom.back() == b.path);
    // and again for the model that failed before: the latch of B does not follow the pass to A
    host.EvaluateFails();
    ApplyRuntimeChange(host, backToA, applied);
    host.Frame();
    CHECK(!host.fail.failed && host.created == 3 && host.builtFrom.back() == a.path);
}

static void NonModelFailureIsNeverErased()
{
    // review M7: a device/global failure is not overwritten by an unavailable runtime and not cleared by a later available one
    FakeHost host;
    uint64_t applied = 0;
    const std::vector<Entry> entries { Folder(L"NR.310.9") };
    Snapshot ok = MakeSnapshot(entries, L"NR.310.9");
    ok.revision = 1;
    Snapshot missing = MakeSnapshot(entries, L"NR.310.8");
    missing.revision = 2;
    Snapshot back = MakeSnapshot(entries, L"NR.310.9");
    back.revision = 3;
    ApplyRuntimeChange(host, ok, applied);
    FailGlobal(host.fail, "the colour codec would not compile");
    ApplyRuntimeChange(host, missing, applied);
    CHECK(host.fail.failed && host.fail.kind == FailureKind::Global);
    CHECK(host.fail.reason == "the colour codec would not compile"); // not replaced by the "not installed" text
    ApplyRuntimeChange(host, back, applied);
    CHECK(host.fail.failed && host.fail.kind == FailureKind::Global);
    host.Frame();
    CHECK(host.created == 0);
    // a global failure wins over a model failure that is already there, and then survives a switch too
    FakeHost other;
    uint64_t applied2 = 0;
    ApplyRuntimeChange(other, ok, applied2);
    other.EvaluateFails();
    FailGlobal(other.fail, "the game's depth or motion vectors could not be made readable");
    CHECK(other.fail.kind == FailureKind::Global);
    ApplyRuntimeChange(other, missing, applied2);
    ApplyRuntimeChange(other, back, applied2);
    CHECK(other.fail.failed && other.fail.kind == FailureKind::Global);
    // the Retry button forgets everything
    ClearAllFailures(other.fail);
    CHECK(!other.fail.failed && other.fail.reason.empty() && other.fail.kind == FailureKind::None);
}

// review M1: the shared capability block must carry the callbacks of the module a create/evaluate is about to call.
struct BindHost
{
    std::vector<std::string> events;
    BlockBinding binding;
    bool bindWorks = true;
    static std::string Name(const std::filesystem::path& p) { return p.parent_path().filename().string(); }
    void Touch(const char* what, const std::filesystem::path& p)
    {
        EnsureBound(binding, p,
                    [&](const std::filesystem::path& q)
                    {
                        events.push_back("bind " + Name(q));
                        return bindWorks;
                    });
        events.push_back(std::string(what) + " " + Name(p));
    }
    void Create(const std::filesystem::path& p) { Touch("create", p); }
    void Evaluate(const std::filesystem::path& p) { Touch("evaluate", p); }
    // every create/evaluate on X must come with X bound: the last bind before it names X
    bool Ordered() const
    {
        std::string bound;
        for (const auto& e : events)
        {
            if (e.rfind("bind ", 0) == 0)
                bound = e.substr(5);
            else if (e.substr(e.find(' ') + 1) != bound)
                return false;
        }
        return true;
    }
    int Binds() const
    {
        int n = 0;
        for (const auto& e : events)
            n += e.rfind("bind ", 0) == 0;
        return n;
    }
};

static void CapabilityBlockFollowsTheModule()
{
    const auto a = Folder(L"NR.310.8").path, b = Folder(L"NR.310.9").path;
    {
        BindHost h;
        h.Create(a);   // scene creates from A
        h.Evaluate(a); // and evaluates: no second bind
        CHECK(h.Binds() == 1);
        h.Create(b);   // a preview creates from B before the scene has switched
        h.Evaluate(a); // the scene evaluates A again: the block must be A's again
        h.Evaluate(a);
        h.Evaluate(b); // the preview evaluates B
        CHECK(h.Ordered());
        CHECK(h.Binds() == 4);
        const std::vector<std::string> first { "bind NR.310.8", "create NR.310.8", "evaluate NR.310.8", "bind NR.310.9", "create NR.310.9" };
        CHECK((std::vector<std::string>(h.events.begin(), h.events.begin() + 5) == first));
    }
    {
        // a bind that fails is not remembered: the next call tries again (and the create still goes ahead, fail open)
        BindHost h;
        h.bindWorks = false;
        h.Create(a);
        h.Create(a);
        CHECK(h.Binds() == 2);
        h.bindWorks = true;
        h.Create(a);
        h.Create(a);
        CHECK(h.Binds() == 3);
        // forgetting the binding (a new block, a device change) binds again
        h.binding.Forget();
        h.Create(a);
        CHECK(h.Binds() == 4);
    }
}

static void TwentyRapidSwitches()
{
    FakeHost host;
    uint64_t applied = 0;
    const std::vector<Entry> entries { Folder(L"NR.310.8"), Folder(L"NR.310.9") };
    const wchar_t* names[] = { L"NR.310.8", L"NR.310.9" };
    Snapshot last;
    for (uint64_t i = 1; i <= 20; ++i)
    {
        last = MakeSnapshot(entries, names[i % 2]);
        last.revision = i;
        ApplyRuntimeChange(host, last, applied);
        host.Frame(); // a frame between the clicks builds the feature of the moment
        host.Frame();
    }
    CHECK(applied == 20);
    CHECK(host.model == last.path);
    CHECK(host.live.size() == 1u);              // exactly one feature alive: the last selection's
    CHECK(host.builtFrom.back() == last.path);
    CHECK(host.created == 20 && host.parked == 19); // every feature but the live one was parked, none twice
    CHECK(host.created == host.parked + static_cast<int>(host.live.size())); // nothing leaked
    CHECK(host.reuseDrops == 20);
}

// The whole chain on a fake clock: clicks reach the Selector (250 ms coalescing), frames poll it and apply the revision to the fake pass.
static void ClicksToFeatures()
{
    FakeHost host;
    uint64_t applied = 0;
    const std::vector<Entry> entries { Folder(L"NR.310.8"), Folder(L"NR.310.9") };
    int resolves = 0;
    Selector selector([&](std::wstring_view r) { ++resolves; return MakeSnapshot(entries, r); });
    const wchar_t* names[] = { L"NR.310.8", L"NR.310.9" };
    uint64_t now = 0;
    const auto frame = [&](const wchar_t* wanted)
    {
        const auto snapshot = selector.Poll(wanted, now);
        ApplyRuntimeChange(host, *snapshot, applied);
        host.Frame();
        now += 16; // a frame every 16 ms
    };
    for (int i = 0; i < 10; ++i)
        frame(names[0]);
    CHECK(host.created == 1 && host.builtFrom.back() == entries[0].path);
    // 21 clicks in a row, one every frame, alternating; the last one asks for NR.310.9
    for (int i = 0; i < 20; ++i)
        frame(names[(i + 1) % 2]);
    frame(names[1]);
    for (int i = 0; i < 40; ++i) // the quiet period passes
        frame(names[1]);
    CHECK(applied == 2);       // exactly one switch for the whole burst
    CHECK(resolves == 2);      // the first poll and the one after the burst
    CHECK(host.created == 2 && host.parked == 1 && host.live.size() == 1u);
    CHECK(host.builtFrom.back() == entries[1].path);
    CHECK(host.model == entries[1].path);
}
static void SessionLockRules()
{
    {
        SessionLock lock;
        CHECK(lock.Reason(false, false) == nullptr); // nothing special: live switching allowed
    }
    {
        SessionLock lock;
        CHECK(lock.Reason(false, true) != nullptr); // the AMD bridge loads its model once
        CHECK(lock.Reason(false, false) == nullptr); // ... and that follows the bridge state
    }
    {
        // the hybrid precision is sticky: on once, locked for the rest of the session even after it is turned off again
        SessionLock lock;
        CHECK(lock.Reason(false, false) == nullptr);
        CHECK(lock.Reason(true, false) != nullptr);
        CHECK(lock.Reason(false, false) != nullptr);
        CHECK(lock.Reason(false, true) != nullptr);
    }
}

int main()
{
    OrderAndFirstRevision();
    SwitchParksTheOldFeature();
    UnavailableAndRecovery();
    OtherFailuresSurvive();
    EvaluateFailureRecoversOnSwitch();
    NonModelFailureIsNeverErased();
    CapabilityBlockFollowsTheModule();
    EvaluateFailureRecoversOnSwitch();
    NonModelFailureIsNeverErased();
    CapabilityBlockFollowsTheModule();
    TwentyRapidSwitches();
    ClicksToFeatures();
    SessionLockRules();
    if (failures == 0)
        std::puts("PASS: NR runtime switch");
    return failures == 0 ? 0 : 1;
}
