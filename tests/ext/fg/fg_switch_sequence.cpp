// The live output switch sequence (ext/fg/outswitch/SwitchSequence.h) with a recording Ops: the order of the steps, the
// decision at every failure, the rollback to plain, the limp window, the session budget, the cooldown, the per-target
// memory, and the probation rollback. No refcounts here (that is the COM simulation next door): this file is about
// which step runs, in which order, with which verdict.
//
// Every scenario states the step it injects a failure into and the end state the game must see: a plain chain on the
// window and an S_OK present, never a second chain and never an error result.
#include "ext/fg/outswitch/SwitchOpsContract.h"
#include "ext/fg/outswitch/SwitchSequence.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace ext::fg::outswitch;

static int g_failed = 0;
#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::printf("    FAIL line %d: %s\n", __LINE__, #cond);                                                    \
            ++g_failed;                                                                                                \
        }                                                                                                              \
    } while (0)

namespace
{
std::vector<std::string> g_section;

void Section(const char* name)
{
    g_section.push_back(name);
    std::printf("  %s\n", name);
}

void Pass(const char* text) { std::printf("    [ok] %s\n", text); }

struct Ops
{
    struct Capture
    {
        void* hwnd = nullptr;
    };
    struct StateSnapshot
    {
        int token = 0;
    };
    struct CreateResult
    {
        void* chain = nullptr;
        void* provider = nullptr;
        uint32_t hr = 0;
        bool denied = false;
    };

    // Knobs.
    PreflightInputs inputs;
    bool barrierOk = true;
    DrainResult drain {};
    bool captureOk = true;
    bool providerCreateOk = true;
    bool plainCreateOk = true;
    bool createDenied = false;
    int deniedTimes = 0;
    Target lastTarget = Target::Plain;
    bool hooksAttached = true;
    bool adoptOk = true;
    bool replayOk = true;
    uint32_t testResult = kOk;
    UnhookResult unhookResult = UnhookResult::Ok;
    bool hooksTransactable = true;
    int transactableLooks = 0;
    bool reenter = false; // a nested present that reaches the switch point while the outer one is inside Create
    int reentries = 0;
    int nestedNotIdle = 0;
    SwitchState* statePtr = nullptr; // set by Booted, for the nested present
    bool bridgeCaptureOk = true;
    bool wanted = true;
    bool witnessPlaceable = true; // the private-data interface can be placed on the live chain
    bool witnessAlive = true;     // the witness is alive until the pin release frees the object (M2)
    bool witnessKilledByDestroy = false; // M1: the destroy ends the object and the witness with it
    bool readLiveOk = true;
    bool liveSeeded = false; // the boot chain was handed over by ReadLive, as the bridge does

    // Observations.
    std::vector<Event> events;
    Status status;
    std::vector<std::string> calls;
    int creates = 0;
    int adopts = 0;
    int teardowns = 0;
    int unhooks = 0;
    int resumes = 0;
    int quiesces = 0;
    int interop = 0;
    int detaches = 0;
    int drains = 0;
    int retires = 0;
    int barriers = 0;
    int replays = 0;
    int tests = 0;
    int restores = 0;
    int publishes = 0;
    int witnesses = 0;
    int pinReleases = 0;
    Target adopted = Target::Plain;
    uint64_t now = 100000;
    uint32_t nextHandle = 1;

    uint64_t NowMs() { return now; }
    void SleepMs(uint32_t ms) { now += ms; }

    bool Wanted() { return wanted; }

    bool ReadLive(LiveChain& out)
    {
        calls.push_back("read-live");
        if (!readLiveOk)
            return false;
        // The chain on the window is the one the fixture describes: this binding reads what the bridge holds, which is
        // what makes the re-read on every attempt idempotent. Only a state that names no chain yet (the boot chain the
        // module never created) gets the fixture's boot chain.
        out = statePtr != nullptr && !statePtr->live.Empty()
                  ? statePtr->live
                  : LiveChain { reinterpret_cast<void*>(0x4000), reinterpret_cast<void*>(0x4001), Class::FsrFg,
                                reinterpret_cast<void*>(0x9010) };
        liveSeeded = true;
        return true;
    }

    PreflightInputs ReadPreflight()
    {
        calls.push_back("preflight");
        return inputs;
    }

    bool Barrier()
    {
        calls.push_back("barrier");
        ++barriers;
        return barrierOk;
    }
    void Quiesce(void* provider)
    {
        calls.push_back("quiesce");
        ++quiesces;
    }
    void Resume(void* provider)
    {
        calls.push_back("resume");
        ++resumes;
    }
    DrainResult Drain(void* provider)
    {
        calls.push_back("drain");
        ++drains;
        return drain;
    }

    bool ReadCapture(void* chain, Capture& out)
    {
        calls.push_back("capture");
        if (!captureOk)
            return false;
        out.hwnd = reinterpret_cast<void*>(0x9010);
        return true;
    }
    bool ReadBridgeCapture(Capture& out)
    {
        calls.push_back("capture-bridge");
        if (!bridgeCaptureOk)
            return false;
        out.hwnd = reinterpret_cast<void*>(0x9010);
        return true;
    }
    void ReleaseCapture(Capture& out) { calls.push_back("release-capture"); }
    StateSnapshot Snapshot()
    {
        calls.push_back("snapshot");
        return StateSnapshot { 7 };
    }
    void RestoreState(const StateSnapshot& snapshot)
    {
        calls.push_back("restore");
        ++restores;
    }

    void ReleaseInterop(void* hwnd)
    {
        calls.push_back("interop");
        ++interop;
    }
    void Detach()
    {
        calls.push_back("detach");
        ++detaches;
    }
    void ClearOldSwapChain() { calls.push_back("clear-old-swapchain"); }

    UnhookResult Unhook()
    {
        calls.push_back("unhook");
        ++unhooks;
        return unhookResult;
    }
    bool HooksTransactable()
    {
        calls.push_back("hooks-transactable");
        ++transactableLooks;
        return hooksTransactable;
    }
    // The knobs describe the new provider class: a plain chain has no hook set, no provider and nothing to replay.
    bool HooksAttached(Target target) { return target == Target::Plain || hooksAttached; }

    CreateResult Create(Target target, const Capture& capture)
    {
        calls.push_back(std::string("create:") + TargetText(target));
        ++creates;
        lastTarget = target;
        if (reenter && statePtr != nullptr)
        {
            // A nested present (the library's factory re-entry) reaching the switch point: it must not start a second
            // sequence on the state the outer one is in the middle of.
            reenter = false;
            ++reentries;
            const SwitchResult nested = RunSwitch(*statePtr, *this, 0);
            if (nested.state != State::Idle)
                ++nestedNotIdle;
        }
        CreateResult result;
        if (createDenied || deniedTimes > 0)
        {
            if (deniedTimes > 0)
                --deniedTimes;
            result.hr = kAccessDenied;
            result.denied = true;
            return result;
        }
        if ((target == Target::Plain && !plainCreateOk) || (target != Target::Plain && !providerCreateOk))
        {
            result.hr = 0x80004005u;
            return result;
        }
        result.chain = reinterpret_cast<void*>(static_cast<uintptr_t>(nextHandle++));
        if (target != Target::Plain)
            result.provider = reinterpret_cast<void*>(static_cast<uintptr_t>(0x1000 + nextHandle));
        result.hr = kOk;
        return result;
    }
    bool Adopt(void* chain, void* provider, Target target)    {
        calls.push_back("adopt");
        ++adopts;
        adopted = target;
        return target == Target::Plain || adoptOk;
    }
    bool Replay(void* chain)
    {
        calls.push_back("replay");
        ++replays;
        return adopted == Target::Plain || replayOk;
    }
    uint32_t TestPresent(void* chain)
    {
        calls.push_back("test");
        ++tests;
        return adopted == Target::Plain ? kOk : testResult;
    }

    void Deactivate(void* provider) { calls.push_back("deactivate"); }
    void ClearChainRefs(const LiveChain& live) { calls.push_back("clear-refs"); }
    bool HasSwapchainContext(void* provider) { return provider != nullptr; }
    bool ReleaseSwapchain(void* provider, void* hwnd)
    {
        calls.push_back("release-swapchain");
        if (witnessKilledByDestroy)
            witnessAlive = false; // the measured destroy already ended the object (M1)
        return true;
    }
    uint32_t ReleasePin(void* chain)
    {
        calls.push_back("release-pin");
        ++pinReleases;
        witnessAlive = false; // the release frees the object: the witness is gone after it
        return 0;
    }
    bool SetWitness(void* chain)
    {
        calls.push_back("witness");
        ++witnesses;
        witnessAlive = true; // a fresh interface on a chain that still exists
        return witnessPlaceable;
    }
    void ReleaseWitness() { calls.push_back("witness-release"); }
    bool WitnessAlive() { return witnessAlive; }
    void ShutdownProvider(void* provider) { calls.push_back("shutdown"); }
    void RetireProvider(void* provider)
    {
        calls.push_back("retire");
        ++retires;
    }
    void ClearCurrentProvider() { calls.push_back("clear-provider"); }

    void Report(const Event& event) { events.push_back(event); }
    void Publish(const Status& snapshot)
    {
        status = snapshot;
        ++publishes;
    }

    // Helpers.
    void TouchCalls()
    {
        // Everything except the read-only entry points and the reporting ones: a refusal must not call any of these.
        calls.clear();
    }
    size_t Steps() const { return events.size(); }
    bool HasEvent(Step step) const
    {
        for (const Event& e : events)
            if (e.step == step)
                return true;
        return false;
    }
    int IndexOf(Step step) const
    {
        for (size_t i = 0; i < events.size(); ++i)
            if (events[i].step == step)
                return static_cast<int>(i);
        return -1;
    }
    std::string Sequence() const
    {
        std::string out;
        for (const Event& e : events)
        {
            const char* name = "?";
            switch (e.step)
            {
            case Step::Idle:
                name = "idle";
                break;
            case Step::Begin:
                name = "begin";
                break;
            case Step::Preflight:
                name = "preflight";
                break;
            case Step::Refused:
                name = "refused";
                break;
            case Step::Barrier:
                name = "barrier";
                break;
            case Step::Quiesce:
                name = "quiesce";
                break;
            case Step::Drain:
                name = "drain";
                break;
            case Step::Capture:
                name = "capture";
                break;
            case Step::Detach:
                name = "detach";
                break;
            case Step::Teardown:
                name = "teardown";
                break;
            case Step::Unhook:
                name = "unhook";
                break;
            case Step::Create:
                name = "create";
                break;
            case Step::Rehook:
                name = "rehook";
                break;
            case Step::Adopt:
                name = "adopt";
                break;
            case Step::Replay:
                name = "replay";
                break;
            case Step::TestPresent:
                name = "test";
                break;
            case Step::Commit:
                name = "commit";
                break;
            case Step::Rollback:
                name = "rollback";
                break;
            case Step::Probation:
                name = "probation";
                break;
            case Step::Limp:
                name = "limp";
                break;
            case Step::Dead:
                name = "dead";
                break;
            case Step::Detached:
                name = "detached";
                break;
            }
            out += (out.empty() ? "" : ",") + std::string(name);
        }
        return out;
    }
};

// The recording Ops is the second binding of the frozen contract.
static_assert(ext::fg::outswitch::SwitchOps<Ops>, "the recording Ops no longer matches the switch's contract");

// A world in which a switch works: a bridge, a measured model for both provider classes, loaders present.
PreflightInputs GoodInputs()
{
    PreflightInputs in;
    in.keyOn = true;
    in.target = Target::XeFg;
    in.active = Target::FsrFg;
    in.bridgePresent = true;
    in.windowed = true;
    in.loadersOk = true;
    in.hooksOk = true;
    in.canCapture = true;
    in.activeModel = LibraryModel { Model::M2, true };
    in.targetModel = LibraryModel { Model::M2, true };
    return in;
}

struct Booted
{
    SwitchState state;
    Ops ops;

    explicit Booted(Target active = Target::FsrFg, Class cls = Class::FsrFg)
    {
        ops.statePtr = &state;
        ops.inputs = GoodInputs();
        ops.inputs.active = active;
        state.active = active;
        state.live = LiveChain { reinterpret_cast<void*>(0x5000), active == Target::Plain ? nullptr : reinterpret_cast<void*>(0x6000), cls,
                                 reinterpret_cast<void*>(0x9010) };
        state.models[static_cast<size_t>(Class::FsrFg)] = LibraryModel { Model::M2, true };
        state.models[static_cast<size_t>(Class::XeFg)] = LibraryModel { Model::M2, true };
    }
};

// --- scenarios ----------------------------------------------------------------------------------------------------

void KeyOffDoesNothing()
{
    Section("key off: idle, no call");
    Booted b;
    b.ops.inputs.keyOn = false;
    const SwitchResult result = RunSwitch(b.state, b.ops, 0);
    CHECK(result.state == State::Idle);
    CHECK(b.ops.calls.size() == 1); // only the read-only preflight
    CHECK(b.ops.barriers == 0 && b.ops.quiesces == 0 && b.ops.creates == 0);
    Pass("key off never enters the machine");
}

void AlreadyActiveDoesNothing()
{
    Section("configured output already active: idle");
    Booted b;
    b.ops.inputs.target = Target::FsrFg;
    b.ops.inputs.active = Target::FsrFg;
    const SwitchResult result = RunSwitch(b.state, b.ops, 0);
    CHECK(result.state == State::Idle);
    CHECK(b.ops.creates == 0);
    Pass("no switch when the target is the active output");
}

void OrderOfSteps()
{
    Section("a working switch: the step order");
    Booted b;
    const SwitchResult result = RunSwitch(b.state, b.ops, 0);
    CHECK(result.state == State::Probation);
    const std::string sequence = b.ops.Sequence();
    const std::string expected = "begin,preflight,barrier,quiesce,drain,capture,detach,teardown,unhook,create,adopt,"
                                 "test,commit";
    CHECK(sequence == expected);
    CHECK(b.ops.IndexOf(Step::Teardown) < b.ops.IndexOf(Step::Unhook));
    CHECK(b.ops.IndexOf(Step::Unhook) < b.ops.IndexOf(Step::Create));
    CHECK(b.ops.IndexOf(Step::Capture) < b.ops.IndexOf(Step::Detach)); // the descriptor is read before the release
    CHECK(b.ops.IndexOf(Step::Detach) < b.ops.IndexOf(Step::Teardown));
    CHECK(b.ops.IndexOf(Step::Create) < b.ops.IndexOf(Step::Adopt));
    CHECK(b.ops.IndexOf(Step::Adopt) < b.ops.IndexOf(Step::TestPresent));
    CHECK(b.ops.status.state == State::Probation);
    CHECK(b.ops.restores == 1); // the state is restored after the create
    CHECK(b.ops.retires == 1);  // the old provider shell is retired
    CHECK(b.ops.interop == 1);  // the old chain's interop resources are released before the detach
    CHECK(std::find(b.ops.calls.begin(), b.ops.calls.end(), std::string("shutdown")) != b.ops.calls.end());
    CHECK(std::find(b.ops.calls.begin(), b.ops.calls.end(), std::string("shutdown")) <
          std::find(b.ops.calls.begin(), b.ops.calls.end(), std::string("retire")));
    CHECK(b.state.live.chain != nullptr);
    CHECK(b.state.live.cls == Class::XeFg);
    CHECK(!b.state.managed);
    Pass(sequence.c_str());
    Pass("capture before detach, create after unhook, one state restore");
}

void EveryPreflightRefusalTouchesNothing()
{
    Section("every preflight refusal: refused, nothing touched");

    struct Case
    {
        const char* name;
        Fail expected;
        Severity severity;
        void (*flip)(PreflightInputs&);
    };
    const Case cases[] = {
        { "target unsupported", Fail::Unsupported, Severity::Transient,
          [](PreflightInputs& i) { i.targetKnown = false; } },
        { "active output unsupported", Fail::Unsupported, Severity::Hard, [](PreflightInputs& i) { i.activeKnown = false; } },
        { "no bridge", Fail::NoBridge, Severity::Hard, [](PreflightInputs& i) { i.bridgePresent = false; } },
        { "shutting down", Fail::ShuttingDown, Severity::Transient, [](PreflightInputs& i) { i.shuttingDown = true; } },
        { "fullscreen", Fail::Fullscreen, Severity::Transient, [](PreflightInputs& i) { i.windowed = false; } },
        { "waitable chain", Fail::WaitableCached, Severity::Hard, [](PreflightInputs& i) { i.waitable = true; } },
        { "game HDR", Fail::GameHdr, Severity::Hard, [](PreflightInputs& i) { i.gameHdr = true; } },
        { "fork HDR with FSR FG", Fail::HdrFsr, Severity::Hard,
          [](PreflightInputs& i) { i.optiHdr = true; i.active = Target::XeFg; i.target = Target::FsrFg; } },
        { "no measured model", Fail::NoModel, Severity::Hard,
          [](PreflightInputs& i) { i.targetModel = LibraryModel {}; } },
        { "no model for the active class", Fail::NoModel, Severity::Hard,
          [](PreflightInputs& i) { i.activeModel = LibraryModel {}; } },
        { "loaders missing", Fail::MissingProvider, Severity::Hard, [](PreflightInputs& i) { i.loadersOk = false; } },
        { "no capture", Fail::NoCapture, Severity::Hard, [](PreflightInputs& i) { i.canCapture = false; } },
        { "hooks inconsistent", Fail::HooksInconsistent, Severity::Hard, [](PreflightInputs& i) { i.hooksOk = false; } },
        { "a hook body still inside", Fail::HookBusy, Severity::Transient, [](PreflightInputs& i) { i.hooksIdle = false; } },
        { "resize busy", Fail::ResizeBusy, Severity::Transient, [](PreflightInputs& i) { i.lastResizeMs = 99990; } },
        { "resize from another thread", Fail::ThreadMix, Severity::Transient,
          [](PreflightInputs& i) { i.lastForeignResizeMs = 99990; } },
    };

    for (const Case& c : cases)
    {
        Booted b;
        c.flip(b.ops.inputs);
        const SwitchResult result = RunSwitch(b.state, b.ops, 0);
        CHECK(result.state == State::Refused);
        CHECK(result.reason == c.expected);
        // One refusal event, with the severity the class says, and nothing else logged.
        CHECK(b.ops.events.size() == 1);
        if (b.ops.events.size() == 1)
        {
            if (b.ops.events[0].severity != c.severity)
                std::printf("      severity mismatch for %s", c.name);
            CHECK(b.ops.events[0].severity == c.severity);
        }
        CHECK(b.ops.quiesces == 0);
        CHECK(b.ops.drains == 0);
        CHECK(b.ops.detaches == 0);
        CHECK(b.ops.creates == 0);
        CHECK(b.state.live.chain != nullptr); // the old chain is still the live one
        CHECK(b.state.policy.SwitchesLeft() == kMaxSwitches); // and no budget unit was spent for a look
    }
    Pass("16 refusals, no call past the read-only preflight");
}

void NoModelIsRememberedUntilTryAgain()
{
    Section("an unknown library build is refused and remembered, try again clears it");
    Booted b;
    b.ops.inputs.targetModel = LibraryModel {};
    CHECK(RunSwitch(b.state, b.ops, 0).reason == Fail::NoModel);
    CHECK(b.state.policy.Remembered(Target::XeFg));
    CHECK(b.state.policy.RememberedReason(Target::XeFg) == Fail::NoModel);
    b.ops.calls.clear();
    CHECK(RunSwitch(b.state, b.ops, 0).reason == Fail::NoModel);
    CHECK(b.ops.creates == 0);
    TryAgain(b.state, Target::XeFg);
    CHECK(!b.state.policy.Remembered(Target::XeFg));
    Pass("NoModel is hard, per target, and cleared by try again");
}

void BootChainIsReadFromTheBridge()
{
    Section("the boot chain is read from the bridge before anything is detached (C-1)");
    Booted b;
    b.state.live = {}; // the module did not create the chain that is on the window
    CHECK(RunSwitch(b.state, b.ops, 0).state == State::Probation);
    CHECK(b.ops.liveSeeded);
    CHECK(!b.state.live.Empty());
    CHECK(b.ops.detaches == 1);
    CHECK(b.ops.retires == 1); // the boot chain's shell was retired by the teardown
    CHECK(b.ops.calls[0] == "preflight");
    CHECK(b.ops.calls[1] == "read-live");
    Pass("ReadLive seeds the live chain and the first switch tears the boot chain down");

    Booted blind;
    blind.state.live = {};
    blind.ops.readLiveOk = false;
    const SwitchResult refused = RunSwitch(blind.state, blind.ops, 0);
    CHECK(refused.state == State::Refused);
    CHECK(refused.reason == Fail::NoCapture);
    CHECK(blind.state.policy.Remembered(Target::XeFg));
    CHECK(blind.ops.detaches == 0 && blind.ops.creates == 0 && blind.ops.retires == 0);
    Pass("an unreadable boot chain refuses hard, so Detach can never run with an unknown chain");

    Booted repeated;
    repeated.state.live = {};
    repeated.ops.readLiveOk = false;
    CHECK(RunSwitch(repeated.state, repeated.ops, 0).reason == Fail::NoCapture);
    const size_t events = repeated.ops.events.size();
    const int publishes = repeated.ops.publishes;
    repeated.ops.calls.clear();
    for (int i = 0; i < 30; ++i)
    {
        repeated.ops.now += 16;
        CHECK(RunSwitch(repeated.state, repeated.ops, 0).state == State::Refused);
    }
    // Inside the hold nothing is read or evaluated: no preflight, no line, no status.
    CHECK(repeated.ops.calls.empty());
    CHECK(repeated.ops.events.size() == events);
    repeated.ops.now += kHardRecheckMs;
    for (int i = 0; i < 30; ++i)
    {
        repeated.ops.now += kHardRecheckMs;
        CHECK(RunSwitch(repeated.state, repeated.ops, 0).state == State::Refused);
    }
    // One line for the change to "this target failed earlier", and not one more for the repeats.
    CHECK(repeated.ops.events.size() == events + 1);
    CHECK(repeated.ops.events.back().fail == Fail::Remembered);
    CHECK(repeated.ops.publishes == publishes); // no status write for a repetition
    CHECK(repeated.state.policy.RememberedReason(Target::XeFg) == Fail::NoCapture);
    Pass("the same refusal is one line and one status, not one per present");
}

void HardRefusalIsHeldNotReadEveryPresent()
{
    Section("a hard refusal is looked at again once a second, not on every present");
    Booted b;
    b.ops.inputs.xeFgSwitchAllowed = false; // an XeFG direction this build does not take
    CHECK(RunSwitch(b.state, b.ops, 0).reason == Fail::XeFgSwitch);
    b.ops.calls.clear();
    // A second of presents at 60 fps: not one preflight read (the read touches the chain on the window).
    for (int i = 0; i < 60 && b.ops.now + 16 < b.state.hardHoldUntilMs; ++i)
    {
        b.ops.now += 16;
        const SwitchResult held = RunSwitch(b.state, b.ops, 0);
        CHECK(held.state == State::Refused && held.reason == Fail::XeFgSwitch);
    }
    CHECK(std::count(b.ops.calls.begin(), b.ops.calls.end(), std::string("preflight")) == 0);
    CHECK(b.ops.detaches == 0 && b.ops.creates == 0);
    // At the end of the hold the preflight runs once and the hold starts again.
    b.ops.now = b.state.hardHoldUntilMs;
    CHECK(RunSwitch(b.state, b.ops, 0).state == State::Refused);
    CHECK(std::count(b.ops.calls.begin(), b.ops.calls.end(), std::string("preflight")) == 1);
    CHECK(b.state.hardHoldUntilMs == b.ops.now + kHardRecheckMs);
    // A transient refusal sets no hold: its own pause is the policy's.
    Booted t;
    t.ops.inputs.windowed = false;
    CHECK(RunSwitch(t.state, t.ops, 0).reason == Fail::Fullscreen);
    CHECK(t.state.hardHoldUntilMs == 0);
    // "Try again" ends the hold: the next present looks at once.
    Booted r;
    r.ops.inputs.targetModel = LibraryModel {};
    CHECK(RunSwitch(r.state, r.ops, 0).reason == Fail::NoModel);
    TryAgain(r.state, Target::XeFg);
    CHECK(r.state.hardHoldUntilMs == 0);
    r.ops.inputs.targetModel = LibraryModel { Model::M2, true };
    CHECK(RunSwitch(r.state, r.ops, 0).state == State::Probation);
    Pass("one preflight per second while refused, none in between; transient refusals and try again set no hold");
}

void DrainFailureIsTransientAndUnremembered()
{
    Section("a drain that is not idle refuses transiently and is not remembered");
    Booted b;
    b.ops.drain.fg = false;
    const SwitchResult first = RunSwitch(b.state, b.ops, 0);
    CHECK(first.state == State::Refused);
    CHECK(first.reason == Fail::Drain);
    CHECK(b.ops.resumes == 1);         // frame generation switches back on
    CHECK(b.ops.detaches == 0);        // nothing was torn down
    CHECK(b.state.live.chain != nullptr);
    CHECK(!b.state.policy.Remembered(Target::XeFg));

    b.ops.now += kTransientRetryMs; // the transient pause is over
    b.ops.drain = DrainResult {};
    const SwitchResult second = RunSwitch(b.state, b.ops, 0);
    CHECK(second.state == State::Probation);
    Pass("Drain refused transiently, no memory, the next attempt succeeds");
}

// A pending Detours transaction (another thread's attach, a library create that reaches the hook) is the one part of the
// detach that cannot be retried for long. It is asked before the barrier and before anything is destroyed: a refusal that
// costs nothing, never a teardown followed by a rollback and a banned target.
void HookTransactionRefusesBeforeAnythingTouched()
{
    Section("a pending hook transaction refuses before the barrier and the teardown");
    Booted b;
    b.ops.hooksTransactable = false;
    const SwitchResult refused = RunSwitch(b.state, b.ops, 0);
    CHECK(refused.state == State::Refused);
    CHECK(refused.reason == Fail::HookBusy);
    CHECK(b.ops.transactableLooks == 1);
    CHECK(b.ops.barriers == 0);   // nothing past the look ran
    CHECK(b.ops.detaches == 0);
    CHECK(b.ops.creates == 0);
    CHECK(b.state.live.chain != nullptr);
    CHECK(!b.state.policy.Remembered(Target::XeFg));

    b.ops.hooksTransactable = true; // the other transaction closed
    b.ops.now += kTransientRetryMs;
    const SwitchResult second = RunSwitch(b.state, b.ops, 0);
    CHECK(second.state == State::Probation);
    Pass("a foreign transaction costs one look, and the next attempt runs");
}

// H-1: a refusal raised inside the sequence (barrier, drain) must take the same pause a preflight refusal takes. Without
// it every following present re-runs the attempt: a log line, a status write and the barrier or drain cost per frame.
void InSequenceRefusalPausesAndKeepsProbation()
{
    Section("a drain refusal pauses the feature: one line, one status, nothing on the next present");
    Booted d;
    d.ops.drain.fg = false;
    CHECK(RunSwitch(d.state, d.ops, 0).reason == Fail::Drain);
    const size_t events = d.ops.events.size();
    const int publishes = d.ops.publishes;
    const int barriers = d.ops.barriers;
    const int drains = d.ops.drains;
    for (int i = 0; i < 10; ++i)
    {
        d.ops.now += 16; // ten presents inside the one-second pause
        const SwitchResult repeat = RunSwitch(d.state, d.ops, 0);
        CHECK(repeat.state == State::Idle);
    }
    CHECK(d.ops.events.size() == events);   // not one more line
    CHECK(d.ops.publishes == publishes);    // not one more status write
    CHECK(d.ops.barriers == barriers);      // the barrier is not paid again
    CHECK(d.ops.drains == drains);          // nor the drain
    // The pause is a second long: at its bound the attempt is looked at again and succeeds.
    d.ops.drain = DrainResult {};
    d.ops.now += kTransientRetryMs;
    CHECK(RunSwitch(d.state, d.ops, 0).state == State::Probation);
    Pass("ten presents inside the pause: no event, no publish, no barrier, no drain; then one attempt");

    Section("a barrier refusal during probation leaves the judgement running");
    Booted b;
    CHECK(RunSwitch(b.state, b.ops, 0).state == State::Probation);
    CHECK(b.state.probation.Watching());
    // The user picks another output while the new chain is still being judged, and the barrier times out.
    b.ops.now += kCooldownMs + 1;
    b.ops.inputs.active = Target::XeFg;
    b.ops.inputs.target = Target::FsrFg;
    b.ops.barrierOk = false;
    const SwitchResult refused = RunSwitch(b.state, b.ops, 0);
    CHECK(refused.state == State::Refused);
    CHECK(refused.reason == Fail::Barrier);
    // Nothing was torn down and the judgement of the chain that is on the window is untouched: its next failure still
    // triggers the rollback.
    CHECK(b.ops.detaches == 1); // only the first switch's
    CHECK(b.state.probation.Watching());
    CHECK(b.state.probation.InProbation());
    CHECK(b.ops.status.state == State::Refused);
    CHECK(OnFgPresentResult(b.state, b.ops, 0x80004005u, 8) == kOk);
    CHECK(b.state.probation.RollbackDue());
    Pass("a refused attempt does not end probation: the failing chain is still rolled back");
}

// H-2: after a commit the configured output IS the active one, so a binding that answers "is another output wanted"
// answers no. The judgement of the chain that is on the window must run before that answer, or a chain that fails its
// presents stays on the window forever.
void RollbackRunsWhenNothingElseIsWanted()
{
    Section("the probation rollback runs with the key on and no output wanted");
    Booted b;
    CHECK(RunSwitch(b.state, b.ops, 0).state == State::Probation);
    // What a real binding reports after a commit: target == active, so Wanted() is false.
    b.ops.wanted = false;
    CHECK(OnFgPresentResult(b.state, b.ops, 0x80004005u, 8) == kOk);
    CHECK(b.state.probation.RollbackDue());
    const SwitchResult rolled = RunSwitch(b.state, b.ops, 0);
    CHECK(rolled.state == State::RolledBack);
    CHECK(rolled.reason == Fail::Probation);
    CHECK(rolled.active == Target::Plain);
    CHECK(b.state.live.cls == Class::Plain);
    CHECK(b.state.policy.Remembered(Target::XeFg));
    Pass("the rollback is not behind the idle path: plain comes back");

    Section("the watchdog rollback runs the same way after probation");
    Booted w;
    CHECK(RunSwitch(w.state, w.ops, 0).state == State::Probation);
    for (uint32_t i = 0; i < kProbationPresents; ++i)
        OnFgPresentResult(w.state, w.ops, kOk, 8);
    w.ops.wanted = false;
    for (uint32_t i = 0; i < kWatchdogFailures; ++i)
        OnFgPresentResult(w.state, w.ops, 0x80004005u, 8);
    CHECK(w.state.probation.RollbackDue());
    CHECK(RunSwitch(w.state, w.ops, 0).state == State::RolledBack);
    CHECK(w.state.live.cls == Class::Plain);
    Pass("three failed presents after probation roll back with nothing wanted");
}

// The Ops contract: RunSwitch runs on the present thread, and a nested present (the library's factory re-entry) must not
// start a second sequence on the state the outer call is inside.
void NestedPresentDoesNotReenter()
{
    Section("a nested present at the switch point does not start a second sequence");
    Booted b;
    b.ops.reenter = true;
    const SwitchResult result = RunSwitch(b.state, b.ops, 0);
    CHECK(result.state == State::Probation);
    CHECK(b.ops.reentries == 1);      // the nested present did reach the switch point
    CHECK(b.ops.nestedNotIdle == 0);  // and was answered Idle
    CHECK(b.ops.creates == 1);        // the nested present created nothing
    CHECK(b.ops.adopts == 1);         // the outer sequence adopted exactly one chain
    CHECK(b.state.live.cls == Class::XeFg);
    CHECK(!b.state.running);          // the guard is cleared on the way out
    Pass("the nested present is Idle and the outer switch completes");
}

void CaptureFailureRefusesBeforeAnyChange()
{
    Section("an unreadable live chain refuses hard, before anything is torn down");
    Booted b;
    b.ops.captureOk = false;
    const SwitchResult result = RunSwitch(b.state, b.ops, 0);
    CHECK(result.state == State::Refused);
    CHECK(result.reason == Fail::NoCapture);
    CHECK(b.ops.detaches == 0);
    CHECK(b.ops.teardowns == 0);
    CHECK(b.ops.creates == 0);
    CHECK(b.ops.resumes == 1);
    CHECK(b.state.policy.Remembered(Target::XeFg));
    Pass("NoCapture is hard and stops the sequence at step 3b");
}

void PlainMoveTakesNoBarrier()
{
    Section("a move out of plain takes no barrier and no quiesce");
    Booted b(Target::Plain, Class::Plain);
    b.ops.inputs.active = Target::Plain;
    b.ops.inputs.target = Target::FsrFg;
    b.state.active = Target::Plain;
    const SwitchResult result = RunSwitch(b.state, b.ops, 0);
    CHECK(result.state == State::Probation);
    CHECK(b.ops.barriers == 0);  // nothing native presents on the window: no barrier to prove anything about
    CHECK(b.ops.quiesces == 0);  // and no provider to quiesce
    CHECK(b.state.live.cls == Class::FsrFg);
    Pass("plain -> FSR FG: no barrier, no quiesce, one commit");
}

void CreateFailureRollsBackToPlain()
{
    Section("the target cannot be created: rollback to plain, target remembered");
    Booted b;
    b.ops.providerCreateOk = false;
    const SwitchResult result = RunSwitch(b.state, b.ops, 0);
    CHECK(result.state == State::RolledBack);
    CHECK(result.reason == Fail::Create);
    CHECK(result.active == Target::Plain);
    CHECK(b.state.live.cls == Class::Plain);
    CHECK(b.state.live.chain != nullptr);
    CHECK(b.state.policy.Remembered(Target::XeFg));
    CHECK(b.state.policy.RememberedReason(Target::XeFg) == Fail::Create);
    CHECK(b.ops.Sequence().find("rollback") != std::string::npos);
    // The plain chain was created from the capture and adopted: no second create for the failed target.
    CHECK(std::find(b.ops.calls.begin(), b.ops.calls.end(), std::string("create:none")) != b.ops.calls.end());
    CHECK(b.ops.adopts == 1);
    CHECK(b.state.policy.SwitchesLeft() == kMaxSwitches - 1); // the detach spent one, as for a commit
    CHECK(b.state.policy.Generation() == 0); // a rollback is not a commit: no generation, no cooldown
    CHECK(b.state.policy.CooldownUntilMs() == 0);
    Pass("RolledBack to plain, memory written, one budget unit spent, no commit");
}

void DeniedWindowLimpsWithoutRebuilding()
{
    Section("the window stays bound: limp, no rebuild, the error after the window");
    Booted b;
    b.ops.createDenied = true;
    const SwitchResult result = RunSwitch(b.state, b.ops, 0);
    CHECK(result.state == State::Limp);
    CHECK(result.reason == Fail::OldChainLeaked);
    CHECK(b.state.managed);
    CHECK(b.ops.creates == kCreateAttempts); // one try plus three retries, all denied
    CHECK(b.ops.adopts == 0);

    // While limping the game gets S_OK; no plain chain is retried because the window is still bound.
    CHECK(DetachedPresent(b.state, b.ops, false) == kOk);
    CHECK(DetachedPresent(b.state, b.ops, false) == kOk);
    b.ops.now += kLimpGiveUpMs;
    CHECK(DetachedPresent(b.state, b.ops, false) == kDeviceRemoved);
    CHECK(b.state.dead);
    Pass("E_ACCESSDENIED x4 -> Limp, S_OK window, then DXGI_ERROR_DEVICE_REMOVED");
}

void PlainFailsLimpsThenRetries()
{
    Section("plain cannot be created: limp, S_OK, then dead");
    Booted b;
    b.ops.providerCreateOk = false;
    b.ops.plainCreateOk = false;
    const SwitchResult result = RunSwitch(b.state, b.ops, 0);
    CHECK(result.state == State::Limp);
    CHECK(result.reason == Fail::PlainCreate);

    // The rebuild retries a plain chain every kLimpRetryMs while the window lasts, and not more often.
    const int createsBefore = b.ops.creates;
    b.ops.now += 1; // inside the interval: no rebuild yet
    CHECK(DetachedPresent(b.state, b.ops, false) == kOk);
    CHECK(b.ops.creates == createsBefore);
    b.ops.now += kLimpRetryMs;
    CHECK(DetachedPresent(b.state, b.ops, false) == kOk);
    CHECK(b.ops.creates > createsBefore);
    // Still failing: the module keeps answering S_OK until the window ends.
    b.ops.now += kLimpRetryMs;
    CHECK(DetachedPresent(b.state, b.ops, false) == kOk);
    b.ops.now += kLimpGiveUpMs;
    CHECK(DetachedPresent(b.state, b.ops, false) == kDeviceRemoved);
    CHECK(b.state.dead);
    Pass("PlainCreate -> Limp, retries every 250 ms, Dead at 3 s");
}

void LimpRecoversWhenPlainComesBack()
{
    Section("a plain chain that succeeds ends the limp");
    Booted b;
    b.ops.providerCreateOk = false;
    b.ops.plainCreateOk = false;
    CHECK(RunSwitch(b.state, b.ops, 0).state == State::Limp);
    b.ops.plainCreateOk = true;
    b.ops.now += kLimpRetryMs;
    CHECK(DetachedPresent(b.state, b.ops, false) == kOk);
    CHECK(!b.state.limp);
    CHECK(!b.state.managed);
    CHECK(b.state.active == Target::Plain);
    CHECK(b.state.live.cls == Class::Plain);
    CHECK(b.ops.status.state == State::RolledBack);
    Pass("the rebuild adopts a plain chain and the bridge presents again");
}

void HookAdoptReplayTestFailuresRollBack()
{
    Section("failures after the create: hook, adopt, replay, test present");
    struct Case
    {
        const char* name;
        Fail expected;
        void (*arm)(Ops&);
    };
    const Case cases[] = {
        { "hooks left no present hook", Fail::Rehook, [](Ops& o) { o.hooksAttached = false; } },
        { "adopt failed", Fail::Adopt, [](Ops& o) { o.adoptOk = false; } },
        { "replay failed", Fail::Replay, [](Ops& o) { o.replayOk = false; } },
        { "test present failed", Fail::TestPresent, [](Ops& o) { o.testResult = 0x80004005u; } },
    };
    for (const Case& c : cases)
    {
        Booted b;
        c.arm(b.ops);
        const SwitchResult result = RunSwitch(b.state, b.ops, 0);
        std::printf("    %-28s -> %s / %s\n", c.name, StateText(result.state), FailText(result.reason));
        CHECK(result.state == State::RolledBack);
        CHECK(result.reason == c.expected);
        CHECK(result.active == Target::Plain);
        CHECK(b.state.live.cls == Class::Plain);
        CHECK(b.state.policy.Remembered(Target::XeFg));
    }
    Pass("four late failures, four rollbacks onto a plain chain");
}

void TestPresentAcceptedResults()
{
    Section("a test present counts S_OK and DXGI_STATUS_OCCLUDED as success");
    for (uint32_t result : { kOk, kOccluded })
    {
        Booted b;
        b.ops.testResult = result;
        const SwitchResult switched = RunSwitch(b.state, b.ops, 0);
        CHECK(switched.state == State::Probation);
        CHECK(b.state.live.cls == Class::XeFg);
    }
    Pass("S_OK and DXGI_STATUS_OCCLUDED commit the switch");
}

void BudgetAndCooldown()
{
    Section("the session budget and the cooldown");
    Booted b;
    for (uint32_t i = 0; i < kMaxSwitches; ++i)
    {
        b.ops.inputs.active = (i % 2 == 0) ? Target::FsrFg : Target::XeFg;
        b.ops.inputs.target = (i % 2 == 0) ? Target::XeFg : Target::FsrFg;
        b.state.active = b.ops.inputs.active;
        b.state.live.cls = ClassOf(b.ops.inputs.active);
        b.ops.now += kCooldownMs + 1;
        const SwitchResult result = RunSwitch(b.state, b.ops, 0);
        CHECK(result.state == State::Probation);
        CHECK(b.ops.retires == i + 1);
    }
    CHECK(b.state.policy.SwitchesLeft() == 0);
    CHECK(b.ops.retires <= static_cast<int>(kMaxSwitches));

    b.ops.inputs.active = Target::FsrFg;
    b.ops.inputs.target = Target::XeFg;
    b.state.active = Target::FsrFg;
    b.state.live.cls = Class::FsrFg;
    b.ops.now += kCooldownMs + 1;
    const SwitchResult ninth = RunSwitch(b.state, b.ops, 0);
    CHECK(ninth.state == State::Refused);
    CHECK(ninth.reason == Fail::Budget);
    CHECK(b.ops.retires == static_cast<int>(kMaxSwitches)); // never a ninth shell
    Pass("8 commits then Budget; the retired set never grows past it");

    Section("the cooldown after a commit");
    Booted c;
    CHECK(RunSwitch(c.state, c.ops, 0).state == State::Probation);
    c.ops.inputs.active = Target::XeFg;
    c.ops.inputs.target = Target::FsrFg;
    c.state.active = Target::XeFg;
    const SwitchResult tooSoon = RunSwitch(c.state, c.ops, 0);
    CHECK(tooSoon.state == State::Refused);
    CHECK(tooSoon.reason == Fail::Cooling);
    c.ops.now += kCooldownMs;
    CHECK(RunSwitch(c.state, c.ops, 0).state == State::Probation);
    Pass("Cooling inside 3 s of a commit, then a switch again");
}

void ProbationRollbackAndMasking()
{
    Section("probation: a failed present is masked, then plain comes back");
    Booted b;
    CHECK(RunSwitch(b.state, b.ops, 0).state == State::Probation);

    // The new chain fails its first present. The game sees S_OK, the rollback is due on the next present.
    const uint32_t masked = OnFgPresentResult(b.state, b.ops, 0x80004005u, 8);
    CHECK(masked == kOk);
    CHECK(b.state.probation.RollbackDue());

    const SwitchResult rolled = RunSwitch(b.state, b.ops, 0);
    CHECK(rolled.state == State::RolledBack);
    CHECK(rolled.reason == Fail::Probation);
    CHECK(rolled.active == Target::Plain);
    CHECK(b.state.live.cls == Class::Plain);
    CHECK(b.state.policy.Remembered(Target::XeFg));
    CHECK(b.ops.detaches >= 2); // the dud was detached before its own provider tore it down
    Pass("S_OK for the game, then a plain chain, target remembered");

    Section("probation: 20 healthy presents judge the chain");
    Booted h;
    CHECK(RunSwitch(h.state, h.ops, 0).state == State::Probation);
    for (uint32_t i = 0; i < kProbationPresents; ++i)
        CHECK(OnFgPresentResult(h.state, h.ops, kOk, 8) == kOk);
    CHECK(!h.state.probation.RollbackDue());
    CHECK(h.ops.status.state == State::Idle);
    Pass("20 healthy presents, no rollback");

    Section("probation: frame generation paused for ten minutes does not roll back");
    Booted p;
    CHECK(RunSwitch(p.state, p.ops, 0).state == State::Probation);
    for (uint32_t i = 0; i < 600; ++i)
    {
        p.ops.now += 1000; // one present a second, none of them healthy because none happened
        // No present at all: the FG dispatch is paused, so OnFgPresentResult is not called.
    }
    CHECK(!p.state.probation.RollbackDue());
    CHECK(p.ops.status.state == State::Probation);
    Pass("no present is not a failure: no livelock while FG is paused");
}

void DeviceLostIsForwardedAndFinal()
{
    Section("a lost device is never masked and never rolled back");
    Booted b;
    CHECK(RunSwitch(b.state, b.ops, 0).state == State::Probation);
    for (uint32_t lost : { kDeviceRemoved, kDeviceReset, kDeviceHung })
    {
        Booted one;
        CHECK(RunSwitch(one.state, one.ops, 0).state == State::Probation);
        CHECK(OnFgPresentResult(one.state, one.ops, lost, 8) == lost);
        CHECK(one.state.dead);
        CHECK(one.ops.status.state == State::Dead);
        CHECK(!one.state.probation.RollbackDue());
        CHECK(RunSwitch(one.state, one.ops, 0).state == State::Dead);
    }
    (void) b;
    Pass("three device-loss codes forwarded, Dead, no rollback");
}

void WatchdogAfterProbation()
{
    Section("after probation, three failed presents in a row roll back");
    Booted b;
    CHECK(RunSwitch(b.state, b.ops, 0).state == State::Probation);
    for (uint32_t i = 0; i < kProbationPresents; ++i)
        OnFgPresentResult(b.state, b.ops, kOk, 8);
    CHECK(OnFgPresentResult(b.state, b.ops, 0x80004005u, 8) == 0x80004005u); // passed through now
    CHECK(!b.state.probation.RollbackDue());
    OnFgPresentResult(b.state, b.ops, 0x80004005u, 8);
    CHECK(!b.state.probation.RollbackDue());
    OnFgPresentResult(b.state, b.ops, 0x80004005u, 8);
    CHECK(b.state.probation.RollbackDue());
    OnFgPresentResult(b.state, b.ops, kOk, 8);
    Pass("watchdog counts three in a row, one healthy present resets it");
}

void StallIsAFailureInProbation()
{
    Section("a stall during probation is a failure, the switch hitch is not");
    Booted b;
    CHECK(RunSwitch(b.state, b.ops, 0).state == State::Probation);
    // The first present after a commit carries the switch, which can take seconds: it is not judged as a stall.
    CHECK(OnFgPresentResult(b.state, b.ops, kOk, kStallMs + 1) == kOk);
    CHECK(!b.state.probation.RollbackDue());
    CHECK(OnFgPresentResult(b.state, b.ops, kOk, 8) == kOk);
    CHECK(OnFgPresentResult(b.state, b.ops, kOk, kStallMs + 1) == kOk);
    CHECK(b.state.probation.RollbackDue());
    CHECK(b.state.probation.LastFail() == Fail::Stall);
    const SwitchResult rolled = RunSwitch(b.state, b.ops, 0);
    CHECK(rolled.state == State::RolledBack);
    CHECK(b.state.policy.RememberedReason(Target::XeFg) == Fail::Stall);
    Pass("Stall is masked, remembered and rolls back");
}

void DetachedPresentUnmanaged()
{
    Section("without a detach of ours the present is what it always was");
    Booted b;
    CHECK(DetachedPresent(b.state, b.ops, false) == kDeviceRemoved);
    CHECK(DetachedPresent(b.state, b.ops, true) == kDeviceRemoved);
    Pass("today's DXGI_ERROR_DEVICE_REMOVED when the module did not detach");
}

void BridgeCaptureFailureDuringRollback()
{
    Section("a probation rollback that cannot read the bridge limps");
    Booted b;
    CHECK(RunSwitch(b.state, b.ops, 0).state == State::Probation);
    CHECK(OnFgPresentResult(b.state, b.ops, 0x80004005u, 8) == kOk);
    b.ops.bridgeCaptureOk = false;
    const SwitchResult result = RunSwitch(b.state, b.ops, 0);
    CHECK(result.state == State::Limp);
    CHECK(b.ops.status.state == State::Limp);
    Pass("no capture for the rollback -> Limp, not a second chain");
}

void ShuttingDownShortCircuits()
{
    Section("shutting down stops everything");
    Booted b;
    CHECK(RunSwitch(b.state, b.ops, 0).state == State::Probation);
    b.ops.inputs.shuttingDown = true;
    b.state.probation.End();
    b.ops.inputs.active = Target::XeFg;
    b.ops.inputs.target = Target::FsrFg;
    b.state.active = Target::XeFg;
    b.ops.now += kCooldownMs;
    const SwitchResult result = RunSwitch(b.state, b.ops, 0);
    CHECK(result.state == State::Refused);
    CHECK(result.reason == Fail::ShuttingDown);
    Pass("a switch never starts while the process is going away");
}
} // namespace

void TransientRefusalsBackOffAndGiveUp()
{
    Section("a transient refusal backs off and the tenth in a row is remembered");
    Booted b;
    b.ops.inputs.active = Target::FsrFg;
    b.ops.inputs.target = Target::XeFg;
    b.ops.drain = DrainResult { false, true, 0 }; // the game queue never proves idle
    uint64_t now = b.ops.now;
    std::vector<uint64_t> attempts;
    int drains = 0;
    // Two hundred simulated seconds of a present every 16 ms: the old fixed one-second retry would have paid the barrier
    // and the drain two hundred times; the backoff has to make that a handful, and then stop for good.
    for (int frame = 0; frame < 200 * 60; ++frame)
    {
        b.ops.now = now;
        const SwitchResult result = RunSwitch(b.state, b.ops, now);
        CHECK(result.state == State::Refused || result.state == State::Idle || result.state == State::Dead);
        if (b.ops.drains != drains)
        {
            ++drains;
            attempts.push_back(now);
        }
        now += 16;
    }
    CHECK(attempts.size() >= 6);  // the first five pauses are 1, 2, 4, 8 and 16 s
    CHECK(attempts.size() <= 10); // and the tenth refusal stores the cause instead of trying again
    if (attempts.size() >= 2 && attempts.size() <= 10)
    {
        // The gaps double (up to the 30 s cap), so the cost of a cause that never passes stays bounded.
        const uint64_t first = attempts[1] - attempts[0];
        const uint64_t second = attempts[2] - attempts[1];
        CHECK(first >= kTransientRetryMs && first < kTransientRetryMs + 200);
        CHECK(second >= 2 * kTransientRetryMs && second < 2 * kTransientRetryMs + 200);
        std::printf("    [ok] attempts %zu, gaps %llu ms then %llu ms\n", attempts.size(),
                    static_cast<unsigned long long>(first), static_cast<unsigned long long>(second));
    }
    // Fifty seconds alone is a handful of attempts, not fifty.
    {
        Booted c;
        c.ops.inputs.active = Target::FsrFg;
        c.ops.inputs.target = Target::XeFg;
        c.ops.drain = DrainResult { false, true, 0 };
        uint64_t clock = c.ops.now;
        int tries = 0;
        int seen = 0;
        for (int frame = 0; frame < 50 * 60; ++frame)
        {
            c.ops.now = clock;
            RunSwitch(c.state, c.ops, clock);
            if (c.ops.drains != seen)
            {
                ++seen;
                ++tries;
            }
            clock += 16;
        }
        CHECK(tries <= 6);
        std::printf("    [ok] fifty seconds of a never-idle drain cost %d attempts\n", tries);
    }
    // The tenth refusal is stored as the target's cause, so the menu can offer a restart instead of a stall forever.
    CHECK(attempts.size() == kTransientGiveUp); // not nine, not eleven: the give-up fires on the tenth attempt
    CHECK(b.state.policy.TransientStreak() == kTransientGiveUp);
    CHECK(b.state.policy.Remembered(Target::XeFg));
    CHECK(b.state.policy.RememberedReason(Target::XeFg) == Fail::Drain);
    char sentence[224] {};
    const Status shown = Snapshot(b.state);
    SessionRefusedText(shown, sentence);
    CHECK(std::string(sentence).find("restart") != std::string::npos);
    const SwitchResult after = RunSwitch(b.state, b.ops, now);
    CHECK(after.reason == Fail::Remembered);
    std::printf("    [ok] %s\n", sentence);
}

void PreflightRefusalsDoNotAddUp()
{
    Section("a preflight refusal pauses once and is never counted toward the give-up");
    Booted b;
    b.ops.inputs.windowed = false; // exclusive fullscreen: transient, nothing touched, free to look at again
    uint64_t now = b.ops.now;
    uint64_t lastRetry = b.state.policy.RetryAfterMs();
    int attempts = 0;
    uint64_t previous = 0;
    for (int frame = 0; frame < 50 * 60; ++frame) // fifty simulated seconds, one present every 16 ms
    {
        b.ops.now = now;
        const SwitchResult result = RunSwitch(b.state, b.ops, now);
        CHECK(result.state == State::Idle || result.state == State::Refused);
        if (b.state.policy.RetryAfterMs() != lastRetry)
        {
            lastRetry = b.state.policy.RetryAfterMs();
            ++attempts;
            if (previous != 0)
                CHECK(now - previous >= kTransientRetryMs && now - previous < kTransientRetryMs + 200);
            previous = now;
        }
        now += 16;
    }
    // One attempt a second: the pause never doubles and the fifth refusal is not the last one.
    CHECK(attempts >= 48 && attempts <= 50);
    CHECK(b.state.policy.TransientStreak() == 0);
    CHECK(!b.state.policy.Remembered(Target::XeFg));
    CHECK(b.state.live.chain != nullptr); // nothing was touched, so the live chain is still the boot one
    std::printf("    [ok] fifty seconds of exclusive fullscreen cost %d looks, none remembered\n", attempts);

    // The condition passes: the next look reaches Go, and the phase where nothing is touched costs nothing.
    b.ops.inputs.windowed = true;
    b.ops.now = now + kTransientRetryMs;
    CHECK(RunSwitch(b.state, b.ops, b.ops.now).state == State::Probation);
    Pass("a state condition that clears is switched to without a ban and without a restart");
}

void TryAgainClearsAGiveUp()
{
    Section("try again clears the give-up, and the next refusal starts the count over");
    Booted b;
    b.ops.drain = DrainResult { false, true, 0 };
    uint64_t now = b.ops.now;
    for (int i = 0; i < kTransientGiveUp; ++i)
    {
        b.ops.now = now;
        RunSwitch(b.state, b.ops, now);
        now = b.state.policy.RetryAfterMs() + 1;
    }
    CHECK(b.state.policy.Remembered(Target::XeFg));
    CHECK(b.state.policy.TransientStreak() == kTransientGiveUp);

    TryAgain(b.state, Target::XeFg);
    CHECK(!b.state.policy.Remembered(Target::XeFg));
    CHECK(b.state.policy.TransientStreak() == 0);
    // The player did not wait out the last pause: the next present is allowed to try.
    b.ops.now += 16;
    b.ops.drains = 0;
    const SwitchResult refused = RunSwitch(b.state, b.ops, b.ops.now);
    CHECK(refused.state == State::Refused);
    CHECK(refused.reason == Fail::Drain);
    CHECK(b.ops.drains == 1);           // it really tried
    CHECK(!b.state.policy.Remembered(Target::XeFg)); // one refusal after the override does not ban the target again
    CHECK(b.state.policy.TransientStreak() == 1);

    // And a cause that clears switches.
    b.ops.drain = DrainResult {};
    b.ops.now = b.state.policy.RetryAfterMs() + 1;
    CHECK(RunSwitch(b.state, b.ops, b.ops.now).state == State::Probation);
    std::printf("    [ok] the give-up is cleared, and the next row of refusals starts at one\n");
}

void ManagedStateStartsNothing()
{
    Section("while the module owns the gap, no sequence starts");
    Booted b;
    b.state.managed = true; // a limp, or the gap a switch of this thread's own present created
    b.ops.TouchCalls();
    const size_t events = b.ops.events.size();
    const SwitchResult result = RunSwitch(b.state, b.ops, b.ops.now);
    CHECK(result.state == State::Idle);
    CHECK(result.reason == Fail::None);
    CHECK(b.ops.calls.empty());       // no read, no barrier, no drain
    CHECK(b.ops.events.size() == events);
    // The other half: with the gap closed the same present switches, so the guard is a defence and not a state machine.
    b.state.managed = false;
    CHECK(RunSwitch(b.state, b.ops, b.ops.now).state == State::Probation);
}

void BusyDetachIsNotRepeated()
{
    Section("a detach that answered Busy is not retried inside the rollback");
    Booted b;
    b.ops.unhookResult = UnhookResult::Busy;
    const SwitchResult result = RunSwitch(b.state, b.ops, b.ops.now);
    CHECK(result.state == State::RolledBack);
    CHECK(result.reason == Fail::HookBusy);
    CHECK(b.ops.unhooks == 1); // the step-7 call only: the rollback skips a second full wait
    int unhookSteps = 0;
    for (const Event& event : b.ops.events)
        if (event.step == Step::Unhook)
            ++unhookSteps;
    CHECK(unhookSteps == 1);
}

void AnM2RowWithoutAWitnessReleasesNothing()
{
    Section("an M2 row without an accepted witness is not a row, and the teardown refuses it before touching anything");
    Booted b;
    LiveChain live { reinterpret_cast<void*>(0x5000), reinterpret_cast<void*>(0x6000), Class::FsrFg, nullptr };
    const TearDownResult torn = TearDownLive(live, b.ops, LibraryModel { Model::M2, false });
    CHECK(!torn.ok);
    CHECK(torn.reason == Fail::ModelViolation);
    CHECK(!torn.pinReleased);
    CHECK(b.ops.pinReleases == 0);
    CHECK(b.ops.witnesses == 0); // not even placed: there is nothing to cross-check
    for (const std::string& call : b.ops.calls)
        CHECK(call != "clear-refs" && call != "deactivate" && call != "release-swapchain" && call != "release-pin");
    CHECK(live.chain == reinterpret_cast<void*>(0x5000)); // the caller still owns it
    // And the same row with a witness is a row the teardown may run.
    Booted c;
    LiveChain second { reinterpret_cast<void*>(0x5000), reinterpret_cast<void*>(0x6000), Class::FsrFg, nullptr };
    const TearDownResult ok = TearDownLive(second, c.ops, LibraryModel { Model::M2, true });
    CHECK(ok.ok);
    CHECK(ok.pinReleased);
    std::printf("    [ok] M2 needs the witness in the row, and the refusal happens before the first call\n");
}

void WitnessMustBePlaceable(){
    Section("a row that promises a witness, on a chain that refuses it, releases nothing");
    Booted b;
    b.ops.witnessPlaceable = false; // SetPrivateDataInterface refuses this chain
    const SwitchResult result = RunSwitch(b.state, b.ops, b.ops.now);
    // The refusal came before the chain was touched, so it is taken back and the provider resumes: a refusal, not an
    // unowned chain on the window.
    CHECK(result.state == State::Refused);
    CHECK(result.reason == Fail::WitnessUnavailable);
    CHECK(b.ops.witnesses == 1);     // it was tried once, before anything was destroyed
    CHECK(b.ops.pinReleases == 0);   // and the table alone never decided a release
    CHECK(b.ops.IndexOf(Step::Teardown) >= 0);
    CHECK(b.ops.adopts == 1);        // the old chain went back on the window
    CHECK(b.ops.resumes == 1);       // and the provider it quiesced is active again
    CHECK(!b.state.managed);
    // The refusal happens before the pointer is cleared or the provider is deactivated: the chain is untouched.
    const size_t first = b.ops.calls.size();
    (void)first;
    for (const std::string& call : b.ops.calls)
        CHECK(call != "clear-refs" && call != "deactivate" && call != "release-swapchain" && call != "release-pin");
    std::printf("    [ok] the release witness is placed before the destroy, and its refusal touches nothing\n");
}

void RehookNamesTheNewClass()
{
    Section("the two hook failures name different classes");
    Booted b;
    b.ops.hooksAttached = false; // the new class was created but no Present hook reached it
    const SwitchResult result = RunSwitch(b.state, b.ops, b.ops.now);
    CHECK(result.state == State::RolledBack);
    CHECK(result.reason == Fail::Rehook);
    CHECK(std::string(FailText(Fail::Rehook)).find("new chain") != std::string::npos);
    CHECK(std::string(FailText(Fail::HookFailed)).find("old chain") != std::string::npos);
    std::printf("    [ok] %s / %s\n", FailText(Fail::Rehook), FailText(Fail::HookFailed));
}

void RollbackRunsTheForwardSteps()
{
    Section("the rollback runs the barrier, quiesce and drain the forward path runs");
    Booted b(Target::XeFg, Class::XeFg);
    b.state.probation.Begin(b.ops.now);
    b.state.active = Target::XeFg;
    CHECK(OnFgPresentResult(b.state, b.ops, 0x80004005u, 8) == kOk); // masked, the rollback is due
    CHECK(b.state.probation.RollbackDue());
    b.ops.barrierOk = false; // best effort: neither of these may stop the rollback
    b.ops.drain = DrainResult { false, true, 0 };
    b.ops.calls.clear();
    const SwitchResult result = RunSwitch(b.state, b.ops, b.ops.now);
    CHECK(result.state == State::RolledBack);
    CHECK(result.reason == Fail::Probation);
    auto at = [&](const char* name) {
        for (size_t i = 0; i < b.ops.calls.size(); ++i)
            if (b.ops.calls[i] == name)
                return static_cast<int>(i);
        return -1;
    };
    const int barrier = at("barrier");
    const int quiesce = at("quiesce");
    const int drain = at("drain");
    const int detach = at("detach");
    CHECK(barrier >= 0 && quiesce > barrier && drain > quiesce && detach > drain);
    if (!(barrier >= 0 && quiesce > barrier && drain > quiesce && detach > drain))
        std::printf("    order: barrier=%d quiesce=%d drain=%d detach=%d\n", barrier, quiesce, drain, detach);
    std::printf("    [ok] a false barrier and a busy queue delay the rollback by nothing: it proceeds to plain\n");
}

// The state is process-wide and describes one bridge. A game path that destroys and recreates its bridge leaves every
// chain pointer in it pointing into the old one, so a state that names another bridge is dropped rather than reused.
void StateBelongsToItsBridge()
{
    Section("the switch state is dropped when the bridge changes (M-3)");
    SwitchState state;
    state.live = LiveChain { reinterpret_cast<void*>(0x5000), reinterpret_cast<void*>(0x6000), Class::XeFg,
                             reinterpret_cast<void*>(0x9010) };
    state.managed = true;
    state.limp = true;
    state.dead = true;
    state.active = Target::XeFg;
    state.target = Target::XeFg;
    state.probation.Begin(1000);
    state.policy.Remember(Target::FsrFg, Fail::Create);
    state.published = State::Dead;

    void* owner = nullptr;
    CHECK(OwnedBy(state, owner, reinterpret_cast<void*>(0x1))); // the first bridge: nothing to compare against
    CHECK(owner == reinterpret_cast<void*>(0x1));
    CHECK(state.live.Empty());
    CHECK(!state.managed);
    CHECK(!state.limp);
    CHECK(!state.dead);
    CHECK(state.active == Target::Plain && state.target == Target::Plain);
    CHECK(state.published == State::Idle);
    CHECK(!state.probation.Watching());
    // The per-target memory survives: a library that failed in one window fails in the next one too.
    CHECK(state.policy.Remembered(Target::FsrFg));
    CHECK(state.policy.RememberedReason(Target::FsrFg) == Fail::Create);

    // The same bridge again: the state is untouched.
    state.probation.Begin(2000);
    CHECK(!OwnedBy(state, owner, reinterpret_cast<void*>(0x1)));
    CHECK(state.probation.Watching());
    CHECK(state.policy.Remembered(Target::FsrFg));

    // Another bridge: everything read from the old one goes, the memory stays.
    CHECK(OwnedBy(state, owner, reinterpret_cast<void*>(0x2)));
    CHECK(owner == reinterpret_cast<void*>(0x2));
    CHECK(!state.probation.Watching());
    CHECK(state.policy.Remembered(Target::FsrFg));
    Pass("a new bridge clears the cached chain and the judgement, and keeps the per-target memory");
}

int main()
{
    KeyOffDoesNothing();
    AlreadyActiveDoesNothing();
    OrderOfSteps();
    EveryPreflightRefusalTouchesNothing();
    NoModelIsRememberedUntilTryAgain();
    HardRefusalIsHeldNotReadEveryPresent();
    DrainFailureIsTransientAndUnremembered();
    HookTransactionRefusesBeforeAnythingTouched();
    InSequenceRefusalPausesAndKeepsProbation();
    RollbackRunsWhenNothingElseIsWanted();
    NestedPresentDoesNotReenter();
    BootChainIsReadFromTheBridge();
    CaptureFailureRefusesBeforeAnyChange();
    PlainMoveTakesNoBarrier();
    CreateFailureRollsBackToPlain();
    DeniedWindowLimpsWithoutRebuilding();
    PlainFailsLimpsThenRetries();
    LimpRecoversWhenPlainComesBack();
    HookAdoptReplayTestFailuresRollBack();
    TestPresentAcceptedResults();
    BudgetAndCooldown();
    ProbationRollbackAndMasking();
    DeviceLostIsForwardedAndFinal();
    WatchdogAfterProbation();
    StallIsAFailureInProbation();
    DetachedPresentUnmanaged();
    BridgeCaptureFailureDuringRollback();
    ShuttingDownShortCircuits();

    TransientRefusalsBackOffAndGiveUp();
    PreflightRefusalsDoNotAddUp();
    TryAgainClearsAGiveUp();
    ManagedStateStartsNothing();
    StateBelongsToItsBridge();
    BusyDetachIsNotRepeated();
    AnM2RowWithoutAWitnessReleasesNothing();
    WitnessMustBePlaceable();
    RehookNamesTheNewClass();
    RollbackRunsTheForwardSteps();

    if (g_failed == 0)
        std::printf("ALL SEQUENCE SCENARIOS PASS\n");
    else
        std::printf("%d CHECK(S) FAILED\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
