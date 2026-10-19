// Dynamic MFG glue (ext/fg/DynamicMfgGlue.h): the tick body and the DLSSGOptions guard that ran only inside the DLL, here with
// a fake world (clock, GPU and feature facts, a log sink that can throw), the real CustomOptional (sliced from Config.h by
// the Prepare script), the real ext::FgConfig and the real sl::DLSSGOptions. Each case is one frame at a time, the way
// DLSSG_Common::SetOptions drives the seams: build the options from the configured count, run the seam, let the plugin
// report what it was given.
#include "dynamic_mfg_harness.h"

#include "ext/fg/DynamicMfgGlue.h"
#include "ext/fg/FgConfig.h"

#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace ext::fg;

namespace
{
// The three upstream Config members the glue touches.
struct TestConfig
{
    CustomOptional<float> FramerateLimit { 0.0f };
    CustomOptional<int> FGDLSSGInterpolationCount { 1 }; // generated frames: 1 = 2x
    bool followsGame = false;
    bool FGFollowsGame() const { return followsGame; }
};

// The three upstream State members the glue touches.
struct TestState
{
    double lastFGFrameTime = 0.0;
    int dlssgDetectedInterpolationCount = 0;
    bool fgChanged = false;
};

struct World
{
    TestConfig cfg;
    ext::FgConfig fg;
    TestState st;
    uint64_t now = 100000;
    bool isDx12 = true, dlssgOut = true, isNvidia = true, dmfg = true, isPaused = false;
    bool gpuIsKnown = true; // false: the GPU has not been identified (yet); nvidia() and architecture() are not to be trusted
    uint32_t arch = 0x190; // Ada
    int maxGen = 5;        // the Ada unlock is on: up to 6x
    unsigned unlocked = 5;
    double refreshHz = 240.0;
    // what the glue did with the world
    int locks = 0, refreshReads = 0;
    std::vector<std::pair<bool, std::string>> logs;
    // fault injection
    bool throwOnRead = false, throwOnLock = false, throwOnLog = false;

    TestConfig& config() { return cfg; }
    ext::FgConfig& fgConfig() { return fg; }
    TestState& state() { return st; }
    uint64_t nowMs() { return now; }
    double readRefreshHz()
    {
        ++refreshReads;
        return refreshHz;
    }
    bool dx12() { return isDx12; }
    bool dlssgOutputActive() { return dlssgOut; }
    bool gpuKnown() { return gpuIsKnown; }
    bool nvidia() { return isNvidia; }
    uint32_t architecture() { return arch; }
    int maxGenerated()
    {
        if (throwOnRead)
            throw std::bad_alloc();
        return maxGen;
    }
    bool dmfgSupported() { return dmfg; }
    bool paused() { return isPaused; }
    unsigned unlockedMax() { return unlocked; }
    int lock()
    {
        if (throwOnLock)
            throw std::bad_alloc();
        ++locks;
        return 0;
    }
    void log(bool warn, const std::string& text)
    {
        if (throwOnLog)
            throw std::bad_alloc();
        logs.push_back({ warn, text });
    }

    int Lines(const char* needle, int warn = -1) const
    {
        int n = 0;
        for (const auto& l : logs)
            n += l.second.find(needle) != std::string::npos && (warn < 0 || l.first == (warn != 0));
        return n;
    }
};

struct Rig
{
    World w;
    AutoMfgGlue glue;
    sl::DLSSGOptions opt;
    bool forceDmfg = false;     // [DLSSG] ForceDMFG
    bool pluginFollows = true;  // the plugin reports the count it was given (one frame later)
    int gameGenerated = 2;      // the count the game asks for in Frame Gen "Default" (Config::FGInterpolationCount)
    // The Streamline state AfterDlssgSetOptions reads after each frame (off unless a case turns `observing` on).
    bool observing = false;
    bool obsQueryOk = true;
    uint32_t obsStatus = 0;     // DLSSGState::status
    int obsPresented = -1;      // DLSSGState::numFramesActuallyPresented, -1 = the factor of the frame (a healthy plugin)

    Rig() { w.fg.autoMfg = true; }

    int Count() { return w.cfg.FGDLSSGInterpolationCount.value_or_default(); }
    std::optional<int> Saved() { return w.cfg.FGDLSSGInterpolationCount.value_for_config(); }

    // What DlssgSeams.cpp does after SetOptions: hand the GetState answer to the glue.
    void Observe()
    {
        sl::DLSSGState state {};
        state.status = static_cast<sl::DLSSGStatus>(obsStatus);
        state.numFramesActuallyPresented = obsPresented >= 0 ? (uint32_t) obsPresented : opt.numFramesToGenerate + 1;
        glue.Observe(w, obsQueryOk, state);
    }

    // One game frame of `ms`: DLSSG_Common::SetOptions builds the options from the configured count, the seam adjusts them.
    void Frame(double ms)
    {
        w.now += (uint64_t) std::max(1.0, std::floor(ms));
        w.st.lastFGFrameTime = ms;
        opt = {};
        opt.mode = forceDmfg ? sl::DLSSGMode::eDynamic : sl::DLSSGMode::eOn;
        opt.numFramesToGenerate = (uint32_t) std::clamp(w.cfg.followsGame ? gameGenerated : Count(), 1, w.maxGen);
        glue.Adjust(w, opt);
        if (pluginFollows)
            w.st.dlssgDetectedInterpolationCount = (int) opt.numFramesToGenerate;
        if (observing)
            Observe();
    }
    // The same frame with the two seam steps apart, to see what the tick answered.
    bool FrameSplit(double ms)
    {
        w.now += (uint64_t) std::max(1.0, std::floor(ms));
        w.st.lastFGFrameTime = ms;
        opt = {};
        opt.mode = forceDmfg ? sl::DLSSGMode::eDynamic : sl::DLSSGMode::eOn;
        opt.numFramesToGenerate = (uint32_t) std::clamp(w.cfg.followsGame ? gameGenerated : Count(), 1, w.maxGen);
        const bool controls = glue.Tick(w, opt);
        glue.Guard(w, opt, controls);
        if (pluginFollows)
            w.st.dlssgDetectedInterpolationCount = (int) opt.numFramesToGenerate;
        return controls;
    }
    void Run(int frames, double ms)
    {
        for (int i = 0; i < frames; ++i)
            Frame(ms);
    }
    // Frames until the configured count is no longer `from` (the selector or the self-test changed it); -1 if it never is.
    int RunUntilCountLeaves(int from, double ms, int maxFrames)
    {
        for (int i = 0; i < maxFrames; ++i)
        {
            Frame(ms);
            if (Count() != from)
                return i;
        }
        return -1;
    }
};

// ---------------------------------------------------------------------------------------------------------------------

void SwitchKeepsTheSliderAndIsNotARecovery()
{
    // The ini says 3x. A light scene (20 ms real = 50 fps) presents 150 at 3x against a 240 Hz target: the selector goes up.
    Rig r;
    r.w.cfg.FGDLSSGInterpolationCount.set_from_config(2);
    const int at = r.RunUntilCountLeaves(2, 20.0, 1500);
    CHECK_MSG(at > 0, "the selector never changed the count");
    CHECK(r.Count() == 3);                          // 4x in memory
    CHECK(r.opt.numFramesToGenerate == 3);          // and in the very options of that frame
    CHECK(r.Saved() == std::optional<int>(2));      // Save Settings still stores the slider, not the selector's pick
    CHECK(!r.w.st.fgChanged);                       // an in-place change is not the toggle path
    CHECK(r.glue.EverActive());
    r.Run(600, 20.0);
    CHECK(!r.w.st.fgChanged && r.Saved() == std::optional<int>(2) && r.Count() == 3);
}

void ABrokenChangeRecoversOnceThroughTheTogglePath()
{
    Rig r;
    r.w.cfg.FGDLSSGInterpolationCount.set_from_config(2);
    CHECK(r.RunUntilCountLeaves(2, 20.0, 1500) > 0);
    // The plugin never applies the new count: it keeps reporting 2 generated frames while 3 were requested.
    r.pluginFollows = false;
    r.w.st.dlssgDetectedInterpolationCount = 2;
    int recoveredAt = -1;
    for (int i = 0; i < 300 && recoveredAt < 0; ++i)
    {
        r.Frame(20.0);
        if (r.w.st.fgChanged)
            recoveredAt = i;
    }
    CHECK_MSG(recoveredAt >= 0, "no recovery");
    CHECK(r.opt.numFramesToGenerate == 2);        // back to the last good count in the options of that frame
    CHECK(r.Count() == 2 && r.Saved() == std::optional<int>(2));
    CHECK(r.w.Lines("automatic mode stopped", 1) >= 1);
    // The toggle path consumes the flag; nothing sets it again, and the stopped mode stops asking the controller.
    r.w.st.fgChanged = false;
    r.pluginFollows = true;
    r.Run(5, 20.0);
    const int locks = r.w.locks;
    r.Run(400, 20.0);
    CHECK(!r.w.st.fgChanged);
    CHECK_MSG(r.w.locks == locks, "a stopped mode still ticks the controller (%d locks)", r.w.locks - locks);
    CHECK(r.opt.numFramesToGenerate == 2 && r.Count() == 2);
}

void FrameGenOnDefaultMeansHandsOff()
{
    Rig r;
    r.w.cfg.followsGame = true; // Frame Gen is on Default: the game owns the count
    r.w.cfg.FGDLSSGInterpolationCount.set_from_config(2);
    for (int i = 0; i < 800; ++i)
    {
        r.Frame(20.0);
        CHECK(r.opt.numFramesToGenerate == 2);
    }
    CHECK(r.Count() == 2 && r.Saved() == std::optional<int>(2) && !r.w.st.fgChanged);
    CHECK(r.glue.controller.Status().gate == AutoMfgGate::FollowsGame);
    CHECK(!r.glue.EverActive());
    CHECK(r.w.Lines("Frame Gen is on Default") == 1);
    // It is not latched as closed for good: the user can leave Default at any time.
    r.w.cfg.followsGame = false;
    r.Run(800, 20.0);
    CHECK(r.glue.controller.Status().gate == AutoMfgGate::Open && r.Count() == 3);
}

void TheGuardClampsForStreamlineOnly()
{
    // ForceDMFG on an Ada whose ceiling the unlock opened, the slider at 6x: Streamline gets 4x, the slider keeps 6x.
    Rig r;
    r.w.fg.autoMfg = false;
    r.forceDmfg = true;
    r.w.cfg.FGDLSSGInterpolationCount.set_from_config(5);
    for (int i = 0; i < 20; ++i)
    {
        r.Frame(10.0);
        CHECK(r.opt.mode == sl::DLSSGMode::eDynamic);
        CHECK(r.opt.numFramesToGenerate == 3);
        CHECK(r.Count() == 5);                      // nothing written back: the menu keeps showing the real slider
        CHECK(r.Saved() == std::optional<int>(5));
    }
    CHECK(r.w.Lines("limited to 4x", 1) == 1);      // logged once
    // The warning also tells how to avoid the one side effect: the frame limiter still paces for the slider count (5 generated
    // frames here), so with a frame limit the slider has to be on 4X (the whole line is pinned, numbers included).
    CHECK_MSG(r.w.Lines("DLSSG: automatic multi frame generation is limited to 4x on this card, 5 generated frames were "
                        "requested; using 3 (set the Frame Gen slider to 4X when a frame limit is used)",
                        1) == 1,
              "the clamp warning text changed");
    // ForceDMFG off: the manual 6x is back at once, with nothing to restore.
    r.forceDmfg = false;
    r.Frame(10.0);
    CHECK(r.opt.mode == sl::DLSSGMode::eOn && r.opt.numFramesToGenerate == 5 && r.Count() == 5);
    // Without the unlock (or on a card that never had it) nothing is clamped.
    r.forceDmfg = true;
    r.w.unlocked = 3;
    r.Frame(10.0);
    CHECK(r.opt.numFramesToGenerate == 5);
    // A 4x slider is already at the ceiling.
    r.w.unlocked = 5;
    r.w.cfg.FGDLSSGInterpolationCount = 3;
    r.Frame(10.0);
    CHECK(r.opt.numFramesToGenerate == 3 && r.Count() == 3);
}

void AutomaticCountNeverReachesStreamlineAbove4x()
{
    // The selector just switched (its watch is open, so it leaves the count alone) when the user drags the slider to 6x.
    // The controller still says "I am in charge": that is what keeps the guard clamping this frame, tick before guard.
    Rig r;
    r.w.cfg.FGDLSSGInterpolationCount.set_from_config(2);
    CHECK(r.RunUntilCountLeaves(2, 20.0, 1500) > 0);
    r.w.cfg.FGDLSSGInterpolationCount = 5;
    r.Frame(20.0);
    CHECK(r.opt.numFramesToGenerate == 3);
    CHECK(r.Count() == 5 && r.Saved() == std::optional<int>(5)); // the slider is untouched
    // Once the watch is over the selector takes the count back itself.
    r.Run(100, 20.0);
    CHECK(r.opt.numFramesToGenerate <= 3);
    // The same slider position with the selector off is the user's: 6x stays 6x.
    Rig manual;
    manual.w.fg.autoMfg = false;
    manual.w.cfg.FGDLSSGInterpolationCount.set_from_config(5);
    manual.Frame(20.0);
    CHECK(manual.opt.numFramesToGenerate == 5);
}

void ForcedDynamicModeNeedsStreamlineSupport()
{
    // L1: where Streamline reports dynamic MFG as unsupported the guard turns the mode into the fixed one, so the selector
    // is not blocked by a mode that will not exist.
    Rig r;
    r.forceDmfg = true;
    r.w.dmfg = false;
    r.Run(30, 20.0);
    CHECK(r.opt.mode == sl::DLSSGMode::eOn);
    CHECK(r.glue.controller.Status().gate == AutoMfgGate::Open);
    CHECK(r.w.Lines("unsupported", 1) == 1);
    // Supported: NVIDIA's own mode picks the multiplier and the selector steps aside.
    Rig s;
    s.forceDmfg = true;
    s.w.dmfg = true;
    s.Run(30, 20.0);
    CHECK(s.opt.mode == sl::DLSSGMode::eDynamic);
    CHECK(s.glue.controller.Status().gate == AutoMfgGate::DynamicMode);
    CHECK(!s.glue.EverActive());
}

void RefreshBelow24HzIsUnknownAndRead5sApart()
{
    Rig r;
    r.w.refreshHz = 1.0; // Windows' "unspecified"
    r.w.cfg.FramerateLimit = 224.0f;
    r.Frame(20.0);
    CHECK(r.glue.controller.Status().refresh == 0.0);
    CHECK(r.glue.controller.Status().target == 224.0); // the limit, not a 30 fps floor
    Rig real;
    real.w.refreshHz = 240.0;
    real.Frame(10.0);
    CHECK(real.glue.controller.Status().refresh == 240.0);
    CHECK(real.glue.controller.Status().target == 240.0);
    // cached for 5 s
    real.Run(450, 10.0);
    CHECK(real.w.refreshReads == 1);
    real.Run(100, 10.0);
    CHECK(real.w.refreshReads == 2);
}

void AClosedGateThatCannotReopenStopsTicking()
{
    struct Case
    {
        const char* name;
        void (*apply)(World&);
    };
    const Case cases[] = {
        { "RTX 50", [](World& w) { w.arch = 0x1B0; } },
        { "not NVIDIA", [](World& w) { w.isNvidia = false; } },
        { "not D3D12", [](World& w) { w.isDx12 = false; } },
    };
    for (const auto& c : cases)
    {
        Rig r;
        c.apply(r.w);
        r.Run(2, 20.0); // the full tick, which logs the reason, then the latch
        CHECK_MSG(r.glue.ClosedForGood(), "%s: latched (the observation of the DLS-G state is skipped too)", c.name);
        CHECK_MSG(r.w.Lines("inactive", 1) == 1, "%s: the reason is logged once", c.name);
        const int locks = r.w.locks, reads = r.w.refreshReads;
        r.Run(400, 20.0);
        CHECK_MSG(r.w.locks == locks, "%s: %d controller ticks after the latch", c.name, r.w.locks - locks);
        CHECK_MSG(r.w.refreshReads == reads, "%s: the refresh was read after the latch", c.name);
        CHECK(r.opt.numFramesToGenerate == 1 && r.Count() == 1);

        // The self-test result is still reported: turning the test on is a change of the keys.
        r.w.fg.autoMfgSelfTest = true;
        r.Frame(20.0);
        CHECK_MSG(r.w.Lines("self-test: FAIL (not run", 1) == 1, "%s: the self-test says why it did not run", c.name);
        const int afterTest = r.w.locks;
        r.Run(200, 20.0);
        CHECK_MSG(r.w.locks == afterTest, "%s: latched again after the self-test line", c.name);

        // Both keys off: the cheap exit of everybody who never turned it on (the controller never saw the key off, so
        // switching it on again is one full tick of a gate it already reported: no second reason line).
        r.w.fg.autoMfg = false;
        r.w.fg.autoMfgSelfTest = false;
        r.Run(100, 20.0);
        CHECK(r.w.locks == afterTest);
        r.w.fg.autoMfg = true;
        r.Run(100, 20.0);
        CHECK_MSG(r.w.locks == afterTest + 1, "%s: %d ticks after the keys changed", c.name, r.w.locks - afterTest);
        CHECK(r.w.Lines("inactive", 1) == 1);
    }

    // Frame Gen on Default can end any time: that gate is not latched, every frame goes to the controller.
    Rig d;
    d.w.cfg.followsGame = true;
    d.Run(2, 20.0);
    CHECK(!d.glue.ClosedForGood());
    const int locks = d.w.locks;
    d.Run(50, 20.0);
    CHECK(d.w.locks == locks + 50);
}

void AnUnidentifiedGpuIsNotAClosedGate()
{
    // IdentifyGpu answered with an empty adapter list (it retries on its next call): that must not read as "not NVIDIA" and
    // latch the closed gate for the whole session. Nothing happens until the GPU is known: no tick, no lock, no latch, no line.
    Rig r;
    r.w.isNvidia = false; // what the fake would say about an unknown GPU if it were asked
    r.w.gpuIsKnown = false;
    r.observing = true;
    r.w.cfg.FGDLSSGInterpolationCount.set_from_config(2);
    r.Run(60, 20.0);
    CHECK(r.opt.numFramesToGenerate == 2 && r.Count() == 2);
    CHECK(!r.glue.ClosedForGood() && !r.glue.EverActive());
    CHECK(r.w.Lines("inactive") == 0);
    CHECK_MSG(r.w.locks == 60, "%d locks (only the observation may take one per frame)", r.w.locks); // Observe: the key is on
    // known now and really not NVIDIA: the closed gate is reported once and latched
    r.w.gpuIsKnown = true;
    r.Run(3, 20.0);
    CHECK(r.w.Lines("AutoMFG: inactive, the GPU is not an NVIDIA one", 1) == 1);
    CHECK(r.glue.ClosedForGood());

    // known later and NVIDIA: the selector works
    Rig n;
    n.w.gpuIsKnown = false;
    n.Run(30, 20.0);
    CHECK(!n.glue.EverActive() && n.glue.controller.Status().gate == AutoMfgGate::KeyOff);
    n.w.gpuIsKnown = true;
    n.Run(30, 20.0);
    CHECK(n.glue.EverActive() && n.glue.controller.Status().gate == AutoMfgGate::Open);
}

void KeyOffAfterAnActivationStopsTicking()
{
    // The key goes off after the mode ran: the full tick that sees it says so and restores, then the glue stops ticking until a
    // key changes (it used to take the lock every frame for the rest of the session).
    Rig r;
    r.observing = true;
    r.w.cfg.FGDLSSGInterpolationCount.set_from_config(2);
    r.Run(100, 20.0);
    CHECK(r.glue.EverActive() && !r.glue.ClosedForGood());
    r.w.fg.autoMfg = false;
    r.Frame(20.0);
    CHECK(r.w.Lines("AutoMFG: switched off", 0) == 1);
    CHECK(r.glue.ClosedForGood());
    CHECK(!r.glue.EverActive()); // the cheap exit of everybody who never turned it on takes over
    const int locks = r.w.locks, reads = r.w.refreshReads;
    r.Run(400, 20.0); // the observation of every frame is skipped too: it would take the lock
    CHECK_MSG(r.w.locks == locks, "%d locks in 400 frames with the key off", r.w.locks - locks);
    CHECK(r.w.refreshReads == reads);
    CHECK(r.w.Lines("AutoMFG: switched off", 0) == 1);
    // the key comes back: one full tick, a new activation line, ticking (and observing) again. The observation is proven by what
    // the stored state says (distinctive values, set while the glue was latched, so the stored ones are stale), and the lock
    // count is exact: a tick and an observation per frame. A lock count of "at least 100" would also pass with the tick alone.
    CHECK(r.glue.observed.valid && r.glue.observed.status == 0 && r.glue.observed.presented == 3); // the last one before the latch
    r.obsStatus = 0x5;
    r.obsPresented = 7;
    r.w.fg.autoMfg = true;
    r.Frame(20.0);
    CHECK_MSG(r.glue.observed.valid && r.glue.observed.status == 0x5 && r.glue.observed.presented == 7,
              "the observation did not resume with the key (status %#x presented %u)", r.glue.observed.status,
              r.glue.observed.presented);
    r.obsStatus = 0;
    r.obsPresented = -1;
    r.Run(99, 20.0);
    CHECK(r.w.Lines("AutoMFG: active", 0) == 2);
    CHECK(r.glue.EverActive() && !r.glue.ClosedForGood());
    CHECK_MSG(r.w.locks == locks + 200, "%d locks in 100 frames with the key on (a tick and an observation per frame)",
              r.w.locks - locks);
    CHECK(r.glue.observed.status == 0 && r.glue.observed.presented == (uint32_t) r.opt.numFramesToGenerate + 1);

    // Only the self-test key on while AutoMFG is off: the self-test says why it did not run, once, then the same.
    Rig s;
    s.w.fg.autoMfg = false;
    s.w.fg.autoMfgSelfTest = true;
    s.Run(2, 20.0);
    CHECK(s.w.Lines("self-test: FAIL (not run: AutoMFG is off)", 1) == 1);
    CHECK(s.glue.ClosedForGood());
    const int sl = s.w.locks;
    s.Run(300, 20.0);
    CHECK(s.w.locks == sl);
}

void AFailingTickFailsOpen()
{
    // Something throws before the controller is reached: the options stay the game's, one warning per episode.
    Rig r;
    r.w.cfg.FGDLSSGInterpolationCount.set_from_config(2);
    r.w.throwOnRead = true;
    for (int i = 0; i < 5; ++i)
    {
        r.Frame(20.0);
        CHECK(r.opt.numFramesToGenerate == 2);
    }
    CHECK(r.w.Lines("an error stopped the automatic multiplier", 1) == 1);
    r.w.throwOnRead = false;
    r.Run(3, 20.0);
    r.w.throwOnRead = true;
    r.Run(3, 20.0);
    CHECK_MSG(r.w.Lines("an error stopped the automatic multiplier", 1) == 2, "a new episode logs again");

    // The lock fails (the controller is never reached).
    Rig l;
    l.w.throwOnLock = true;
    l.Run(4, 20.0);
    CHECK(l.opt.numFramesToGenerate == 1);
    CHECK(l.w.Lines("an error stopped the automatic multiplier", 1) == 1);

    // Even the warning fails to log: still nothing escapes.
    Rig m;
    m.w.throwOnRead = true;
    m.w.throwOnLog = true;
    m.Run(4, 20.0);
    CHECK(m.opt.numFramesToGenerate == 1);
}

void ALogFailureDoesNotLoseACommittedSwitch()
{
    // The controller has committed to the switch and opened its watch; if the log lines then fail, the count must still
    // reach the options, or the watch would see the old count and call the change broken (forcing a recovery and a stop).
    Rig r;
    r.w.throwOnLog = true;
    r.w.cfg.FGDLSSGInterpolationCount.set_from_config(2);
    int at = -1;
    bool switchControls = false;
    for (int i = 0; i < 1500 && at < 0; ++i)
    {
        const bool controls = r.FrameSplit(20.0);
        if (r.Count() != 2)
        {
            at = i;
            switchControls = controls;
        }
    }
    CHECK(at > 0);
    CHECK(switchControls); // and the answer "the selector is in charge" survives the failing log, so the guard still clamps
    CHECK(r.opt.numFramesToGenerate == 3 && r.Count() == 3);
    r.Run(300, 20.0);
    CHECK(!r.w.st.fgChanged && !r.glue.controller.Stopped());
    CHECK(r.Count() == 3);
}

void SelfTestEndStateReachesTheCount()
{
    // Key turned off while the self-test sits on its 4x step: the count goes back to the one in use before the test, even
    // though the selector no longer controls (the glue applies a setFactor whatever `controls` says).
    Rig r;
    r.w.fg.autoMfgSelfTest = true;
    r.w.cfg.FGDLSSGInterpolationCount.set_from_config(2); // 3x before the test
    // frames until the cycle reaches 4x (generated = 3)
    int frames = 0;
    while (frames < 3000 && r.Count() != 3)
    {
        r.Frame(20.0);
        ++frames;
    }
    CHECK_MSG(r.Count() == 3, "the self-test never reached 4x");
    r.Run(10, 20.0);
    r.w.fg.autoMfg = false;
    r.Frame(20.0);
    CHECK(r.opt.numFramesToGenerate == 2);
    CHECK(r.Count() == 2 && r.Saved() == std::optional<int>(2));
    CHECK(r.w.Lines("back to the count in use before the test", 0) >= 1);
    CHECK(r.w.Lines("self-test: FAIL (interrupted", 1) == 1);
    r.Run(100, 20.0);
    CHECK(r.opt.numFramesToGenerate == 2 && r.Count() == 2);

    // Frame Gen switched to Default at that moment instead: the game's count stays in the options, and the configured count
    // (unused in Default) still goes back to the one before the test for when the user leaves Default.
    Rig g;
    g.w.fg.autoMfgSelfTest = true;
    g.w.cfg.FGDLSSGInterpolationCount.set_from_config(2);
    frames = 0;
    while (frames < 3000 && g.Count() != 3)
    {
        g.Frame(20.0);
        ++frames;
    }
    CHECK(g.Count() == 3);
    g.Run(10, 20.0);
    g.w.cfg.followsGame = true;
    g.gameGenerated = 1;
    g.Frame(20.0);
    CHECK_MSG(g.opt.numFramesToGenerate == 1, "the glue wrote %d over the game's count", (int) g.opt.numFramesToGenerate);
    CHECK_MSG(g.Count() == 2 && g.Saved() == std::optional<int>(2), "the configured count was left at %d", g.Count());
}

// ---------------------------------------------------------------------------------------------------------------------
// The Streamline observation (status, presented frames) driven through the glue: AfterDlssgSetOptions hands it over, the next
// tick feeds the kill-switch. Frames are 20 ms; the watch after a switch closes on the 50th frame after the switching one,
// and an observation made after frame j is read by the tick of frame j + 1.

// A rig whose selector has just switched up (from 3x to 4x), observed healthy until then; returns after the switching frame.
void SwitchWithObservation(Rig& r)
{
    r.observing = true;
    r.w.cfg.FGDLSSGInterpolationCount.set_from_config(2);
    const int at = r.RunUntilCountLeaves(2, 20.0, 1500);
    CHECK_MSG(at > 0, "the selector never changed the count");
    CHECK(r.Count() == 3 && !r.w.st.fgChanged && r.w.Lines("AutoMFG:", 1) == 0);
}

void ObservedPresentedStuckAtOneIsABrokenChange()
{
    Rig r;
    SwitchWithObservation(r);
    r.obsPresented = 1; // the plugin keeps presenting one frame per real frame, 5 samples and more, from the switching frame on
    r.Observe();        // (the sample of the switching frame itself, taken after its options)
    int at = -1;
    for (int j = 1; j <= 60 && at < 0; ++j)
    {
        r.Frame(20.0);
        if (r.w.st.fgChanged)
            at = j;
    }
    CHECK_MSG(at == 50, "recovery after %d frames (the watch is 50 frames long)", at);
    CHECK(r.w.Lines("presented frames stuck at 1, switch 3x -> 4x; going back to 3x, automatic mode stopped", 1) == 1);
    CHECK(r.opt.numFramesToGenerate == 2 && r.Count() == 2); // back to the last good count, in the very options of that frame
    // once, through the toggle path: nothing sets the flag again and nothing is logged again
    r.w.st.fgChanged = false;
    r.Run(400, 20.0);
    CHECK(!r.w.st.fgChanged);
    CHECK(r.w.Lines("presented frames stuck at 1") == 1 && r.w.Lines("automatic mode stopped") == 1);
}

void ObservedStatusAtTheEndOfAWatchIsABrokenChange()
{
    Rig r;
    SwitchWithObservation(r);
    int at = -1;
    for (int j = 1; j <= 60 && at < 0; ++j)
    {
        if (j == 40)
            r.obsStatus = 0x2; // read by the ticks 41 to 50: the last 10 of the watch, 180 ms
        r.Frame(20.0);
        if (r.w.st.fgChanged)
            at = j;
    }
    CHECK_MSG(at == 50, "recovery after %d frames", at);
    CHECK(r.w.Lines("AutoMFG: Streamline status 0x2, switch 3x -> 4x; going back to 3x, automatic mode stopped", 1) == 1);
    CHECK(r.w.Lines("presented frames stuck") == 0 && r.w.Lines("applied count") == 0);
    CHECK(r.opt.numFramesToGenerate == 2 && r.Count() == 2);
}

void AHealthyObservationLetsTheChangeSettle()
{
    // Status 0 and the presented count of the new factor, the whole watch: settled, nothing to recover.
    Rig r;
    SwitchWithObservation(r);
    r.Run(60, 20.0);
    CHECK(!r.w.st.fgChanged && !r.glue.controller.Stopped());
    CHECK(r.w.Lines("AutoMFG: 4x settled", 0) == 1);
    CHECK(r.w.Lines("AutoMFG:", 1) == 0);
    CHECK(r.Count() == 3);
}

void AStatusThatWasThereBeforeTheSwitchIsNotItsFault()
{
    Rig r;
    r.observing = true;
    r.obsStatus = 0x2; // set before the switch, and it stays
    r.w.cfg.FGDLSSGInterpolationCount.set_from_config(2);
    CHECK(r.RunUntilCountLeaves(2, 20.0, 1500) > 0);
    r.Run(200, 20.0);
    CHECK(!r.w.st.fgChanged && !r.glue.controller.Stopped());
    CHECK(r.w.Lines("Streamline status") == 0);
    CHECK(r.w.Lines("settled", 0) >= 1);
    CHECK(r.Count() == 3); // and the selector goes on working
}

void TheObservationIsOnlyKeptWhereItCanBeUsed()
{
    // Key on: the values arrive as given, status and presented in their own fields.
    Rig s;
    s.obsStatus = 0x2;
    s.obsPresented = 3;
    s.Observe();
    CHECK(s.glue.observed.valid && s.glue.observed.status == 0x2 && s.glue.observed.presented == 3);
    s.obsQueryOk = false; // a failed GetState query is not a sample
    s.Observe();
    CHECK(!s.glue.observed.valid);

    // Only the self-test key on: still watched.
    Rig t;
    t.w.fg.autoMfg = false;
    t.w.fg.autoMfgSelfTest = true;
    t.obsStatus = 0x2;
    t.obsPresented = 3;
    t.Observe();
    CHECK(t.glue.observed.valid && t.glue.observed.status == 0x2);

    // Both keys off and never active: nothing is kept, no lock taken.
    Rig k;
    k.w.fg.autoMfg = false;
    k.obsStatus = 0x2;
    k.obsPresented = 3;
    k.Observe();
    CHECK(!k.glue.observed.valid && k.w.locks == 0);

    // Closed for good (here: not NVIDIA): nothing is kept, no lock taken.
    Rig c;
    c.w.isNvidia = false;
    c.Run(2, 20.0);
    CHECK(c.glue.ClosedForGood());
    const int locks = c.w.locks;
    c.obsStatus = 0x2;
    c.obsPresented = 3;
    c.Observe();
    CHECK(!c.glue.observed.valid && c.w.locks == locks);
}
} // namespace

int main()
{
    SwitchKeepsTheSliderAndIsNotARecovery();
    ABrokenChangeRecoversOnceThroughTheTogglePath();
    FrameGenOnDefaultMeansHandsOff();
    TheGuardClampsForStreamlineOnly();
    AutomaticCountNeverReachesStreamlineAbove4x();
    ForcedDynamicModeNeedsStreamlineSupport();
    RefreshBelow24HzIsUnknownAndRead5sApart();
    AClosedGateThatCannotReopenStopsTicking();
    KeyOffAfterAnActivationStopsTicking();
    AnUnidentifiedGpuIsNotAClosedGate();
    AFailingTickFailsOpen();
    ALogFailureDoesNotLoseACommittedSwitch();
    SelfTestEndStateReachesTheCount();
    ObservedPresentedStuckAtOneIsABrokenChange();
    ObservedStatusAtTheEndOfAWatchIsABrokenChange();
    AHealthyObservationLetsTheChangeSettle();
    AStatusThatWasThereBeforeTheSwitchIsNotItsFault();
    TheObservationIsOnlyKeptWhereItCanBeUsed();
    if (g_failures)
    {
        std::printf("dynamic_mfg_glue: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("dynamic_mfg_glue: all passed\n");
    return 0;
}
