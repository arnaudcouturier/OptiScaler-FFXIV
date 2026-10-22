#pragma once
// The boundary half of the source-level jitter fix ([FFXIV] JitterSource): one Step per DLSS evaluate, called from
// ext::ffxiv::jitter::OnUpscaleBoundary before the calibrate-and-scale engine. Pure logic templated on an environment, like the engine, so
// tests/ext/ffxiv/jitter/jitter_source_controller.cpp runs it against a fake parameter block and a fake clock.
//
// Contract (the tests pin it):
//   - key off and nothing to unwind: nothing else is touched (no clock, no parameter read, no lookup).
//   - the fix is ACTIVE only when the decision table says so (FfxivJitterSourceCore.h: key, executable, guards, hooks, no fall-back, generator
//     normalising). Active: the Jitter.Offset pair of THIS evaluate is replaced by the projected offset of the camera sample it belongs to
//     (X then Y, the raw pair read before the write), and the calibrate-and-scale engine stands down for this evaluate (`standDown`).
//   - a pairing miss is counted. It never writes a guess (no arithmetic on the pair): when the generator recorded the frame (matched by its raw
//     pair, normalised, of ANY age: the same raw pair is the same phase and strength, so the same value) the record's own Halton value is told;
//     otherwise nothing is written and the engine is NOT stood down (it scales as today). A generator-divided value above the engine's 0.55 px
//     limit that no record explains is counted as untellable, and only kUntellableLimit of them within kUntellableWindowMs make the fix fall
//     back (a single one is left to the engine like a small value).
//   - a block that still holds the pair this controller wrote is already told: no pairing, no miss counted, no write, the engine stays out.
//   - frames already drawn on the full lattice are told what was drawn even when the fix has just been switched off (kDrainMs), so a frame is
//     never half of each.
//   - verification: for every paired frame whose generator record is known, the drawn offset is compared with the generator's own value; a
//     run of frames outside the tolerance, or a run of evaluates that match no camera sample, makes the fix fall back for the session (both
//     detours disabled together; switching the key off and on again tries once more).
//
// The environment `Env` provides:
//   bool KeyOn();                                  [FFXIV] JitterSource, resolved
//   HooksView Hooks();                             executable match, guard result, installed; asks for the install (retry rules inside)
//   std::optional<float> GetFloat(Param);          parameter block read; nullopt when absent
//   std::uint32_t GetUInt(Param);                  0 when absent
//   void SetFloat(Param, float);                   the ONLY write (Jitter.Offset.X / .Y)
//   std::uint64_t NowMs();
//   void Log(const std::string&);

#include "ext/ffxiv/jitter/FfxivJitterCore.h"
#include "ext/ffxiv/jitter/FfxivJitterSourceCore.h"

namespace ext::ffxiv::jitter::source
{
struct HooksView
{
    bool buildMatched = false;
    bool guardsOk = false;
    bool installed = false;
};

struct StepResult
{
    bool active = false;     // the fix is active in this evaluate
    bool standDown = false;  // calibrate-and-scale must not touch this evaluate (the fix owns the pair)
    float maxJitter = 0.5f;  // the largest |jitter| the camera object may legitimately hold (the probe's snapshot guard)
};

// A line budget: a session cannot grow its log without bound.
class LineBudget
{
  public:
    explicit LineBudget(std::uint32_t lines) : _left(lines) {}
    bool Take()
    {
        if (_left == 0)
            return false;
        --_left;
        return true;
    }

  private:
    std::uint32_t _left;
};

// The largest |value| the camera object holds while the generator output is divided by `strength`: 0.5 / strength, with a margin.
inline float MaxObjectJitter(float strength)
{
    if (!(strength >= kMinStrength))
        return 0.5f;
    return std::min(0.5f / strength, 0.5f / kMinStrength) * 1.02f + 0.01f;
}

template <class Env> class Controller
{
  public:
    static constexpr std::uint64_t kFirstVerifyLogAt = 120;    // checked frames before the first verification line
    static constexpr std::uint64_t kVerifyLogEveryMs = 30000;    // for the first kVerifyFastLines lines, then kVerifySlowEveryMs
    static constexpr std::uint64_t kVerifySlowEveryMs = 300000;
    static constexpr std::uint32_t kVerifyFastLines = 5;
    static constexpr std::uint64_t kInactiveDelayMs = 2000;    // a transitional reason (hooks not yet in, generator not yet running) is logged after this
    static constexpr std::uint64_t kSeenSettleMs = 5000;       // the first sighting of the generator's call-site objects is reported after this

    StepResult Step(Env& env, SharedState& state, bool correctionKeyOn)
    {
        // Today's cost with the key off: this test and the branch. Nothing was ever switched on, nothing to unwind.
        if (!env.KeyOn())
            return KeyOff(env, state);

        const std::uint64_t now = env.NowMs();
        const HooksView hooks = env.Hooks();
        DecisionInput in;
        in.keyOn = true;
        in.buildMatched = hooks.buildMatched;
        in.guardsOk = hooks.guardsOk;
        in.hooksInstalled = hooks.installed;
        in.fellBack = _fellBack;
        const GeneratorStatus status = state.Status(now);
        in.generatorRecent = status.recent;
        const SeenObjects seen = state.Seen();
        const std::uintptr_t world = state.WorldObject();
        in.worldLearned = world != 0;
        in.worldSeen = SeenContains(seen, world);
        Decision decision = Decide(in);

        // Both detours follow one condition: enabled while the hooks are in place and the fix has not fallen back, disabled otherwise.
        const bool shouldRun = hooks.installed && hooks.buildMatched && hooks.guardsOk && !_fellBack;
        if (shouldRun && !state.Enabled())
        {
            state.Enable(true);
            _engaged = true;
        }
        else if (!shouldRun && state.Enabled())
            SwitchOff(state, now);

        StepResult result;
        result.active = decision.active;
        if (decision.active)
            result.maxJitter = MaxObjectJitter(status.strength);

        // The engine is kept out only of an evaluate whose pair the fix owns: told (written or held). A miss with nothing sensible to tell leaves
        // the pair to the engine, which scales it as today.
        const Told told = TellDrawnOffset(env, state, now, decision.active);
        result.standDown = told == Told::Written || told == Told::Held;

        if (decision.active)
        {
            const Fallback verdict = _watchdog.Verdict();
            if (verdict != Fallback::None)
            {
                _fellBack = true;
                if (_logs.fellBack.Take())
                    env.Log(FormatFellBack(verdict, _watchdog.totals()));
                SwitchOff(state, now);
                decision = { false, Reason::FellBack };
                result.active = false; // this evaluate was handled; from the next one the old behaviour owns the pair
            }
        }

        Report(env, now, decision, status, correctionKeyOn);
        ReportObjects(env, state, seen, world, now);
        return result;
    }

    bool Engaged() const { return _engaged; }
    bool FellBack() const { return _fellBack; }
    bool WasActive() const { return _wasActive; }
    const Watchdog::Totals& Totals() const { return _watchdog.totals(); }
    ToldLattice::Result Lattice() const { return _lattice.Summary(); }

  private:
    // The key is off. If the fix was ever switched on: say so once, turn both detours off, and keep telling frames that were already drawn on
    // the full lattice for kDrainMs.
    StepResult KeyOff(Env& env, SharedState& state)
    {
        if (!_engaged && _drainUntilMs == 0 && !_wasActive && !_fellBack && !_lastTold.valid)
            return {};
        const std::uint64_t now = env.NowMs();
        if (_wasActive && _logs.transitions.Take())
            env.Log(FormatInactive(Reason::KeyOff));
        _wasActive = false;
        _pendingReason = Reason::None;
        _fellBack = false; // switching the key off and on again is the way to try once more
        _watchdog.Reset();
        SwitchOff(state, now);
        StepResult result;
        if (_drainUntilMs != 0 && now >= _drainUntilMs)
            _drainUntilMs = 0;
        if (_drainUntilMs == 0 && !_lastTold.valid)
            return result;
        const Told told = TellDrawnOffset(env, state, now, false);
        result.standDown = told == Told::Written || told == Told::Held;
        return result;
    }

    void SwitchOff(SharedState& state, std::uint64_t now)
    {
        if (state.Enabled())
            state.Enable(false);
        if (_engaged)
        {
            _engaged = false;
            _drainUntilMs = now + kDrainMs;
        }
    }

    // What the told-offset step did with this evaluate's pair.
    enum class Told : std::uint8_t
    {
        Nothing,    // nothing written, nothing held (not active and no frame to drain; or active and nothing to tell: left to the engine)
        Written,    // the pair was replaced
        Held,       // the block still holds the pair this controller wrote: already told
        Untellable  // active, a miss, no record, a value the engine cannot scale: counted (the fix falls back after kUntellableLimit of them)
    };

    struct LastTold
    {
        bool valid = false;
        float x = 0.0f, y = 0.0f;
    };

    // The told-offset write. The NGX pair is read BEFORE the write.
    // Not active: only a frame the generator normalised (drawn on the full lattice) inside the drain window is told, anything else is the
    // old behaviour's. A block that still holds the pair this controller wrote is "held" in every state: it is not a miss, it is never
    // written again and the engine must not scale it (it would take the told pair for a game pair).
    Told TellDrawnOffset(Env& env, SharedState& state, std::uint64_t now, bool active)
    {
        const bool holding = _lastTold.valid;
        const bool draining = !active && now < _drainUntilMs;
        if (!active && !draining && !holding)
            return Told::Nothing;
        const std::optional<float> ngxX = env.GetFloat(Param::JitterX);
        const std::optional<float> ngxY = env.GetFloat(Param::JitterY);
        const RenderSize size = ResolveRenderSize(env.GetUInt(Param::SubrectWidth), env.GetUInt(Param::SubrectHeight), env.GetUInt(Param::Width),
                                                  env.GetUInt(Param::Height));
        std::optional<Match> match;
        if ((active || draining) && ngxX && ngxY && size.width != 0 && size.height != 0)
            match = state.Pair(*ngxX, *ngxY, size.width, size.height, now);
        if (!match && holding && ngxX && ngxY)
        {
            if (*ngxX == _lastTold.x && *ngxY == _lastTold.y)
                return Told::Held;
            _lastTold.valid = false; // the game refreshed the block: the memory of our pair is gone
        }
        if (!match)
            return active ? TellMiss(env, state, now, ngxX, ngxY) : Told::Nothing;
        const std::optional<Generated> generated = state.FindGenerated(*ngxX, *ngxY);
        const bool drawnFull = generated && generated->normalized;
        if (!active && !drawnFull)
            return Told::Nothing;
        if (active)
        {
            if (drawnFull)
                _watchdog.Checked(Deviation(match->pixels, *generated));
            else
                _watchdog.Unverified();
        }
        WritePair(env, match->pixels.x, match->pixels.y, *ngxX);
        return Told::Written;
    }

    // An active evaluate whose pair matched no camera sample. Counted. Told the generator record's value when there is one, of any age (never
    // arithmetic on the pair; the same raw pair means the same phase index and strength, hence the same expected value); otherwise nothing is
    // written: a small value is the old behaviour's and the engine scales it as today, a divided value the engine cannot scale is counted as
    // untellable and left to the engine too (the fix falls back only when that repeats, see Watchdog::Untellable).
    Told TellMiss(Env& env, SharedState& state, std::uint64_t now, const std::optional<float>& ngxX, const std::optional<float>& ngxY)
    {
        _watchdog.Unpaired();
        if (!ngxX || !ngxY)
            return Told::Nothing;
        const std::optional<Generated> record = state.FindGenerated(*ngxX, *ngxY);
        if (record && record->normalized)
        {
            _watchdog.ToldFromRecord();
            WritePair(env, record->expectedX, record->expectedY, *ngxX);
            return Told::Written;
        }
        if (std::fabs(*ngxX) > kMaxReportedPixels || std::fabs(*ngxY) > kMaxReportedPixels)
        {
            _watchdog.Untellable(now);
            return Told::Untellable;
        }
        return Told::Nothing;
    }

    // X then Y; the pair read before is `originalX`. A failed second write puts the game's X back. Remembers the pair as "ours".
    void WritePair(Env& env, float x, float y, float originalX)
    {
        env.SetFloat(Param::JitterX, x);
        try
        {
            env.SetFloat(Param::JitterY, y);
        }
        catch (...)
        {
            try
            {
                env.SetFloat(Param::JitterX, originalX); // a half-written pair is worse than the game's own
            }
            catch (...)
            {
            }
            throw;
        }
        _lastTold = { true, x, y };
        _lattice.Add(x, y);
    }

    void Report(Env& env, std::uint64_t now, const Decision& decision, const GeneratorStatus& status, bool correctionKeyOn)
    {
        if (decision.active)
        {
            _pendingReason = Reason::None;
            if (!_wasActive)
            {
                _wasActive = true;
                _loggedPhases = status.phases;
                _nextVerifyLogMs = 0;
                if (_logs.transitions.Take())
                {
                    env.Log(FormatActive(status.strength, status.phases, status.renderW, status.renderH, status.outW, status.outH));
                    if (correctionKeyOn)
                        env.Log(FormatStandDown());
                }
                NoteSizes(env, status);
            }
            else if (status.phases != 0 && status.phases != _loggedPhases)
            {
                if (_logs.phases.Take())
                    env.Log(FormatPhasesChanged(_loggedPhases, status.phases, status.strength, status.renderW, status.renderH, status.outW, status.outH));
                _loggedPhases = status.phases;
            }
            const Watchdog::Totals& t = _watchdog.totals();
            if (t.checked >= kFirstVerifyLogAt && now >= _nextVerifyLogMs && _logs.verify.Take())
            {
                ++_verifyLines;
                _nextVerifyLogMs = now + (_verifyLines <= kVerifyFastLines ? kVerifyLogEveryMs : kVerifySlowEveryMs);
                env.Log(FormatVerified(t, _lattice.Summary(), status.phases));
                _lattice.Reset(); // the next line shows its own window
            }
            return;
        }

        // Inactive although the key is on: say why. A permanent reason at once; a transitional one (hooks not yet in, the generator not yet
        // running) only when it has lasted; a switch from active at once.
        const bool permanent = decision.reason == Reason::ExecutableNotMatched || decision.reason == Reason::GuardFailed ||
                               decision.reason == Reason::FellBack;
        if (decision.reason != _pendingReason)
        {
            _pendingReason = decision.reason;
            _pendingSince = now;
            _pendingShown = false;
        }
        if (_wasActive)
        {
            _wasActive = false;
            _pendingShown = true;
            if (_logs.transitions.Take())
                env.Log(FormatInactive(decision.reason));
        }
        else if (!_pendingShown && (permanent || now - _pendingSince >= kInactiveDelayMs))
        {
            _pendingShown = true;
            if (_logs.transitions.Take())
                env.Log(FormatInactive(decision.reason));
        }
    }

    // The diagnostics of the world-camera tie: which jitter objects the generator's camera call site is reached with, and, if the world camera's
    // pointer is known but never among them, that the fix cannot act. Written from here (the evaluate thread), not from a detour. The first
    // objects line waits until the world camera is known and the first sighting has had time to settle; a later one is written (within the
    // budget) whenever a new object appears, so a second camera that shows up minutes later is not missed. The "cannot act" line is written
    // once per learned pointer, after the same settle time.
    void ReportObjects(Env& env, SharedState& state, const SeenObjects& seen, std::uintptr_t world, std::uint64_t now)
    {
        if (!state.Enabled())
            return;
        if (seen.count == 0)
            return;
        if (world == 0)
            return;
        if (_seenSince == 0)
            _seenSince = now;
        const bool settled = now - _seenSince >= kSeenSettleMs;
        const std::size_t signature = seen.count + (seen.overflow ? 1000 : 0);
        if (signature != _seenLogged && (_seenLogged != 0 || settled))
        {
            _seenLogged = signature;
            if (_logs.objects.Take())
                env.Log(FormatSeenObjects(seen, world));
        }
        if (settled && !SeenContains(seen, world) && _worldMissLogged != world)
        {
            _worldMissLogged = world;
            if (_logs.worldMiss.Take())
                env.Log(FormatWorldNeverSeen(world, seen));
        }
    }

    void NoteSizes(Env& env, const GeneratorStatus& status)
    {
        const std::uint32_t ngxOutW = env.GetUInt(Param::OutWidth), ngxOutH = env.GetUInt(Param::OutHeight);
        if (ngxOutW != 0 && ngxOutH != 0 && (ngxOutW != status.outW || ngxOutH != status.outH) && _logs.sizes.Take())
            env.Log(FormatSizeDisagreement(status.outW, status.outH, ngxOutW, ngxOutH));
    }

    struct Logs
    {
        LineBudget transitions { 24 }, phases { 32 }, verify { 20 }, fellBack { 4 }, sizes { 4 }, objects { 6 }, worldMiss { 3 };
    };

    Watchdog _watchdog;
    Logs _logs;
    bool _engaged = false;   // the detours were enabled by this controller and not yet switched off
    bool _fellBack = false;
    bool _wasActive = false; // the last decision was "active"
    Reason _pendingReason = Reason::None;
    std::uint64_t _pendingSince = 0;
    bool _pendingShown = false;
    std::uint64_t _drainUntilMs = 0;
    LastTold _lastTold;
    ToldLattice _lattice;
    std::uint32_t _loggedPhases = 0;
    std::uint64_t _nextVerifyLogMs = 0;
    std::uint32_t _verifyLines = 0;
    std::uint64_t _seenSince = 0;     // when the world camera's object and a call-site sighting were first both known
    std::size_t _seenLogged = 0;      // the signature (count, overflow) of the set last reported
    std::uintptr_t _worldMissLogged = 0; // the world pointer the "learned but never seen" line was last written for
};
} // namespace ext::ffxiv::jitter::source
