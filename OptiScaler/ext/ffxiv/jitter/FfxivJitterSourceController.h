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
//   - a pairing miss passes the NGX pair through unchanged and is counted; it never writes a guess.
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
    static constexpr std::uint64_t kVerifyLogEveryMs = 30000;
    static constexpr std::uint64_t kInactiveDelayMs = 2000;    // a transitional reason (hooks not yet in, generator not yet running) is logged after this

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
        result.standDown = decision.active;
        if (decision.active)
            result.maxJitter = MaxObjectJitter(status.strength);

        if (TellDrawnOffset(env, state, now, decision.active))
            result.standDown = true;

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
        return result;
    }

    bool Engaged() const { return _engaged; }
    bool FellBack() const { return _fellBack; }
    bool WasActive() const { return _wasActive; }
    const Watchdog::Totals& Totals() const { return _watchdog.totals(); }

  private:
    // The key is off. If the fix was ever switched on: say so once, turn both detours off, and keep telling frames that were already drawn on
    // the full lattice for kDrainMs.
    StepResult KeyOff(Env& env, SharedState& state)
    {
        if (!_engaged && _drainUntilMs == 0 && !_wasActive && !_fellBack)
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
        if (_drainUntilMs == 0)
            return result;
        if (now >= _drainUntilMs)
        {
            _drainUntilMs = 0;
            return result;
        }
        result.standDown = TellDrawnOffset(env, state, now, false);
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

    // The told-offset write. Returns true when the pair was replaced. The NGX pair is read BEFORE the write; a pairing miss writes nothing.
    // Not active: only a frame the generator normalised (drawn on the full lattice) inside the drain window is told, anything else is the
    // old behaviour's.
    bool TellDrawnOffset(Env& env, SharedState& state, std::uint64_t now, bool active)
    {
        if (!active && now >= _drainUntilMs)
            return false;
        const std::optional<float> ngxX = env.GetFloat(Param::JitterX);
        const std::optional<float> ngxY = env.GetFloat(Param::JitterY);
        const RenderSize size = ResolveRenderSize(env.GetUInt(Param::SubrectWidth), env.GetUInt(Param::SubrectHeight), env.GetUInt(Param::Width),
                                                  env.GetUInt(Param::Height));
        std::optional<Match> match;
        if (ngxX && ngxY && size.width != 0 && size.height != 0)
            match = state.Pair(*ngxX, *ngxY, size.width, size.height, now);
        if (!match)
        {
            if (active)
                _watchdog.Unpaired();
            return false;
        }
        const std::optional<Generated> generated = state.FindGenerated(*ngxX, *ngxY);
        const bool drawnFull = generated && generated->normalized;
        if (!active && !drawnFull)
            return false;
        if (active)
        {
            if (drawnFull)
                _watchdog.Checked(Deviation(match->pixels, *generated));
            else
                _watchdog.Unverified();
        }
        env.SetFloat(Param::JitterX, match->pixels.x);
        try
        {
            env.SetFloat(Param::JitterY, match->pixels.y);
        }
        catch (...)
        {
            try
            {
                env.SetFloat(Param::JitterX, *ngxX); // a half-written pair is worse than the game's own
            }
            catch (...)
            {
            }
            throw;
        }
        return true;
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
                _nextVerifyLogMs = now + kVerifyLogEveryMs;
                env.Log(FormatVerified(t));
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

    void NoteSizes(Env& env, const GeneratorStatus& status)
    {
        const std::uint32_t ngxOutW = env.GetUInt(Param::OutWidth), ngxOutH = env.GetUInt(Param::OutHeight);
        if (ngxOutW != 0 && ngxOutH != 0 && (ngxOutW != status.outW || ngxOutH != status.outH) && _logs.sizes.Take())
            env.Log(FormatSizeDisagreement(status.outW, status.outH, ngxOutW, ngxOutH));
    }

    struct Logs
    {
        LineBudget transitions { 24 }, phases { 32 }, verify { 20 }, fellBack { 4 }, sizes { 4 };
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
    std::uint32_t _loggedPhases = 0;
    std::uint64_t _nextVerifyLogMs = 0;
};
} // namespace ext::ffxiv::jitter::source
