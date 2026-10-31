// The preflight of a live output switch (ext/fg/outswitch/SwitchPolicy.h), on its own: the verdict for every input, the
// class of each refusal, the per-target memory, the cooldown, the budget and the two "a moment ago" windows. The
// thresholds are checked at their boundaries, because each one is a mutation target.
#include "ext/fg/outswitch/SwitchPolicy.h"

#include <cstdint>
#include <cstdio>
#include <string_view>

using namespace ext::fg::outswitch;

static int g_failed = 0;
#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                                                        \
            ++g_failed;                                                                                                \
        }                                                                                                              \
    } while (0)

namespace
{
constexpr uint64_t kNow = 100000;

PreflightInputs Good()
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

struct Case
{
    const char* name;
    Fail reason;
    Severity severity;
    bool remembered;
    void (*flip)(PreflightInputs&);
};

void Verdicts()
{
    std::printf("  verdicts and refusal classes\n");
    const Case cases[] = {
        { "unknown configured output", Fail::Unsupported, Severity::Transient, false,
          [](PreflightInputs& i) { i.targetKnown = false; } },
        { "unknown active output", Fail::Unsupported, Severity::Hard, true, [](PreflightInputs& i) { i.activeKnown = false; } },
        { "not on the bridge", Fail::NoBridge, Severity::Hard, true, [](PreflightInputs& i) { i.bridgePresent = false; } },
        { "a second bridge", Fail::SecondBridge, Severity::Hard, true, [](PreflightInputs& i) { i.secondBridge = true; } },
        { "shutting down", Fail::ShuttingDown, Severity::Transient, false, [](PreflightInputs& i) { i.shuttingDown = true; } },
        { "exclusive fullscreen", Fail::Fullscreen, Severity::Transient, false, [](PreflightInputs& i) { i.windowed = false; } },
        { "waitable chain", Fail::WaitableCached, Severity::Hard, true, [](PreflightInputs& i) { i.waitable = true; } },
        { "game HDR", Fail::GameHdr, Severity::Hard, true, [](PreflightInputs& i) { i.gameHdr = true; } },
        { "fork HDR and FSR FG", Fail::HdrFsr, Severity::Hard, true,
          [](PreflightInputs& i) { i.optiHdr = true; i.active = Target::Plain; i.target = Target::FsrFg; } },
        { "XeFG on the window is not torn down", Fail::XeFgSwitch, Severity::Hard, true,
          [](PreflightInputs& i) { i.xeFgSwitchAllowed = false; i.active = Target::XeFg; i.target = Target::Plain; } },
        { "XeFG as the target is not entered", Fail::XeFgSwitch, Severity::Hard, true,
          [](PreflightInputs& i) { i.xeFgSwitchAllowed = false; i.active = Target::FsrFg; i.target = Target::XeFg; } },
        { "hooks inconsistent", Fail::HooksInconsistent, Severity::Hard, true, [](PreflightInputs& i) { i.hooksOk = false; } },
        { "target library not measured", Fail::NoModel, Severity::Hard, true,
          [](PreflightInputs& i) { i.targetModel = LibraryModel {}; } },
        { "active library not measured", Fail::NoModel, Severity::Hard, true,
          [](PreflightInputs& i) { i.activeModel = LibraryModel {}; } },
        { "target libraries do not load", Fail::MissingProvider, Severity::Hard, true,
          [](PreflightInputs& i) { i.loadersOk = false; } },
        { "live chain unreadable", Fail::NoCapture, Severity::Hard, true, [](PreflightInputs& i) { i.canCapture = false; } },
        { "live chain held by someone else", Fail::RefMismatch, Severity::Transient, false,
          [](PreflightInputs& i) { i.activeRefs = 4; i.expectedRefs = 3; } },
        { "resize 499 ms ago", Fail::ResizeBusy, Severity::Transient, false,
          [](PreflightInputs& i) { i.lastResizeMs = kNow - (kResizeBusyMs - 1); } },
        { "resize on another thread 4999 ms ago", Fail::ThreadMix, Severity::Transient, false,
          [](PreflightInputs& i) { i.lastForeignResizeMs = kNow - (kThreadMixMs - 1); } },
    };

    for (const Case& c : cases)
    {
        Policy policy;
        PreflightInputs in = Good();
        c.flip(in);
        const Verdict verdict = policy.Evaluate(in, kNow);
        std::printf("    %-38s -> %s (%s)\n", c.name, FailText(verdict.reason), SeverityText(verdict.severity));
        CHECK(verdict.kind == VerdictKind::Refuse);
        CHECK(verdict.reason == c.reason);
        CHECK(verdict.severity == c.severity);
        CHECK(verdict.log);
        CHECK(policy.Remembered(in.target) == c.remembered);
    }

    std::printf("  the windows close at their threshold\n");
    {
        Policy policy;
        PreflightInputs in = Good();
        in.lastResizeMs = kNow - kResizeBusyMs;
        CHECK(policy.Evaluate(in, kNow).kind == VerdictKind::Go);
        in.lastResizeMs = kNow - kResizeBusyMs + 1;
        CHECK(policy.Evaluate(in, kNow).reason == Fail::ResizeBusy);
    }
    {
        Policy policy;
        PreflightInputs in = Good();
        in.lastForeignResizeMs = kNow - kThreadMixMs;
        CHECK(policy.Evaluate(in, kNow).kind == VerdictKind::Go);
        in.lastForeignResizeMs = kNow - kThreadMixMs + 1;
        CHECK(policy.Evaluate(in, kNow).reason == Fail::ThreadMix);
    }
    {
        // A clock that went backwards is not "recent".
        Policy policy;
        PreflightInputs in = Good();
        in.lastResizeMs = kNow + 5000;
        CHECK(policy.Evaluate(in, kNow).kind == VerdictKind::Go);
    }
    std::printf("    [ok] the resize windows are open exactly at their bound\n");
}

void Idle()
{
    std::printf("  idle: nothing to do, nothing logged\n");
    {
        Policy policy;
        PreflightInputs in = Good();
        in.keyOn = false;
        const Verdict verdict = policy.Evaluate(in, kNow);
        CHECK(verdict.kind == VerdictKind::Idle);
        CHECK(verdict.reason == Fail::KeyOff);
        CHECK(!verdict.log);
        CHECK(!policy.Remembered(in.target));
    }
    {
        Policy policy;
        PreflightInputs in = Good();
        in.target = in.active;
        const Verdict verdict = policy.Evaluate(in, kNow);
        CHECK(verdict.kind == VerdictKind::Idle);
        CHECK(verdict.reason == Fail::AlreadyActive);
        CHECK(!verdict.log);
    }
    std::printf("    [ok] key off and already active are idle, not refusals\n");
}

void TransientPause()
{
    std::printf("  transient refusals pause, hard ones are remembered\n");
    {
        Policy policy;
        PreflightInputs in = Good();
        in.windowed = false;
        CHECK(policy.Evaluate(in, kNow).reason == Fail::Fullscreen);
        // Inside the pause nothing happens at all, not even a second refusal line.
        CHECK(policy.Evaluate(in, kNow + kTransientRetryMs - 1).kind == VerdictKind::Idle);
        // At the bound the cause is looked at again.
        in.windowed = true;
        CHECK(policy.Evaluate(in, kNow + kTransientRetryMs).kind == VerdictKind::Go);
        // Still bad: the same refusal again, but only one log line per distinct reason.
        in.windowed = false;
        Policy second;
        CHECK(second.Evaluate(in, kNow).log);
        CHECK(!second.Evaluate(in, kNow + kTransientRetryMs).log);
        CHECK(!second.Remembered(in.target));
    }
    {
        Policy policy;
        PreflightInputs in = Good();
        in.bridgePresent = false;
        CHECK(policy.Evaluate(in, kNow).severity == Severity::Hard);
        CHECK(policy.Remembered(in.target));
        CHECK(policy.RememberedReason(in.target) == Fail::NoBridge);
        // A hard refusal is not put on a timer: the memory is what keeps the switch away.
        const Verdict again = policy.Evaluate(in, kNow + 1);
        CHECK(again.kind == VerdictKind::Refuse);
        CHECK(again.reason == Fail::NoBridge);
        CHECK(!again.log); // the same reason, logged once
    }
    std::printf("    [ok] transient: paused then retried; hard: remembered, logged once\n");
}

void Memory()
{
    std::printf("  the per-target memory\n");
    Policy policy;
    PreflightInputs in = Good();
    policy.Remember(Target::XeFg, Fail::Create);
    CHECK(policy.Remembered(Target::XeFg));
    CHECK(policy.RememberedReason(Target::XeFg) == Fail::Create);
    CHECK(!policy.Remembered(Target::FsrFg));
    CHECK(!policy.Remembered(Target::Plain));
    {
        PreflightInputs remembered = Good();
        const Verdict verdict = policy.Evaluate(remembered, kNow);
        CHECK(verdict.kind == VerdictKind::Refuse);
        CHECK(verdict.reason == Fail::Remembered);
        CHECK(verdict.severity == Severity::Hard);
    }
    // The other target is still free.
    {
        PreflightInputs other = Good();
        other.active = Target::XeFg;
        other.target = Target::FsrFg;
        CHECK(policy.Evaluate(other, kNow).kind == VerdictKind::Go);
    }
    policy.ClearMemory(Target::XeFg);
    CHECK(!policy.Remembered(Target::XeFg));
    {
        PreflightInputs back = Good();
        CHECK(policy.Evaluate(back, kNow).kind == VerdictKind::Go);
    }
    std::printf("    [ok] one target failing leaves the others alone; try again clears exactly one\n");
}

void CooldownAndBudget()
{
    std::printf("  the cooldown and the session budget\n");
    {
        Policy policy;
        PreflightInputs in = Good();
        policy.Spend(); // the detach of a switch: the budget pays before anything is torn down
        policy.Committed(kNow);
        CHECK(policy.Generation() == 1);
        CHECK(policy.SwitchesLeft() == kMaxSwitches - 1);
        CHECK(policy.Evaluate(in, kNow).reason == Fail::Cooling);
        CHECK(policy.Evaluate(in, kNow + kCooldownMs - 1).reason == Fail::Cooling);
        CHECK(policy.Evaluate(in, kNow + kCooldownMs).kind == VerdictKind::Go);
        // A commit does not spend a second unit.
        policy.Committed(kNow + kCooldownMs);
        CHECK(policy.SwitchesLeft() == kMaxSwitches - 1);
    }
    {
        // A spend without a commit is a switch that rolled back: the unit is not refunded.
        Policy policy;
        policy.Spend();
        CHECK(policy.SwitchesLeft() == kMaxSwitches - 1);
        CHECK(policy.Generation() == 0);
    }
    {
        Policy policy;
        PreflightInputs in = Good();
        for (uint32_t i = 0; i < kMaxSwitches; ++i)
        {
            CHECK(policy.Evaluate(in, kNow + i * (kCooldownMs + 1)).kind == VerdictKind::Go);
            policy.Spend();
            policy.Committed(kNow + i * (kCooldownMs + 1));
        }
        CHECK(policy.SwitchesLeft() == 0);
        CHECK(policy.Generation() == kMaxSwitches);
        const uint64_t later = kNow + kMaxSwitches * (kCooldownMs + 1);
        const Verdict verdict = policy.Evaluate(in, later);
        CHECK(verdict.kind == VerdictKind::Refuse);
        CHECK(verdict.reason == Fail::Budget);
        CHECK(verdict.severity == Severity::Hard);
        // A refused attempt does not spend budget, and try again does not refund it.
        policy.ClearMemory(Target::XeFg);
        CHECK(policy.SwitchesLeft() == 0);
    }
    std::printf("    [ok] 3 s of cooldown, 8 spends, the ninth refused, nothing refunded\n");

    std::printf("  the retired provider shells are capped independently of the budget\n");
    {
        // A rollback retires the old shell and the dud, so two switches can fill the set of eight while the budget still
        // has units: the preflight has to refuse on the retired count as well.
        Policy policy;
        PreflightInputs in = Good();
        in.retiredShells = kMaxSwitches;
        const Verdict verdict = policy.Evaluate(in, kNow);
        CHECK(verdict.kind == VerdictKind::Refuse);
        CHECK(verdict.reason == Fail::Budget);
        CHECK(policy.SwitchesLeft() == kMaxSwitches); // nothing is spent by a refusal
        in.retiredShells = kMaxSwitches - 1;
        CHECK(policy.Evaluate(in, kNow).kind == VerdictKind::Go); // the last unit of the cap is still available
    }
    std::printf("    [ok] the retired set is capped at eight, whatever the budget says\n");
}

void FirstCauseAndSessionText()
{
    std::printf("  the stored failure keeps the first cause\n");
    Policy policy;
    policy.Remember(Target::XeFg, Fail::Create);
    policy.Remember(Target::XeFg, Fail::Remembered);
    policy.Remember(Target::FsrFg, Fail::Stall);
    CHECK(policy.RememberedReason(Target::XeFg) == Fail::Create); // not overwritten by a later "remembered"
    CHECK(policy.RememberedReason(Target::FsrFg) == Fail::Stall);
    CHECK(policy.Remembered(Target::XeFg) && policy.Remembered(Target::FsrFg));
    policy.ClearMemory(Target::XeFg);
    CHECK(policy.RememberedReason(Target::XeFg) == Fail::None);
    policy.Remember(Target::XeFg, Fail::Probation); // after "try again", the new cause is stored
    CHECK(policy.RememberedReason(Target::XeFg) == Fail::Probation);
    std::printf("    [ok] Remembered never overwrites the cause; try again clears it and a new cause is stored\n");

    std::printf("  the hard refusal is remembered for the session, and says so\n");
    Policy second;
    PreflightInputs in = Good();
    in.gameHdr = true;
    CHECK(second.Evaluate(in, kNow).reason == Fail::GameHdr);
    CHECK(second.Remembered(Target::XeFg));
    // The cause clears (the user turns game-native HDR off), the memory does not: no auto-clear.
    in.gameHdr = false;
    const Verdict remembered = second.Evaluate(in, kNow + 1);
    CHECK(remembered.kind == VerdictKind::Refuse);
    CHECK(remembered.reason == Fail::Remembered);
    CHECK(second.RememberedReason(Target::XeFg) == Fail::GameHdr);

    Status status;
    status.state = State::Refused;
    status.reason = second.RememberedReason(Target::XeFg);
    char text[224] {};
    SessionRefusedText(status, text);
    const std::string_view sentence(text);
    CHECK(sentence.find("refused for this session:") == 0);
    CHECK(sentence.find("restart the game to try again") != std::string_view::npos);
    CHECK(sentence.find("game-native HDR") != std::string_view::npos);
    std::printf("    [ok] %s\n", text);
}

void ModelAndHooksRules()
{
    std::printf("  which class needs a measured model and which needs hooks\n");
    {
        // Plain needs no model at either end.
        Policy policy;
        PreflightInputs in = Good();
        in.active = Target::Plain;
        in.target = Target::Plain; // already active: idle
        in.activeModel = LibraryModel {};
        in.targetModel = LibraryModel {};
        CHECK(policy.Evaluate(in, kNow).kind == VerdictKind::Idle);

        PreflightInputs toPlain = Good();
        toPlain.target = Target::Plain;
        toPlain.targetModel = LibraryModel {};
        CHECK(policy.Evaluate(toPlain, kNow).kind == VerdictKind::Go);
    }
    {
        // A provider chain needs hooks; a plain to plain move does not.
        Policy policy;
        PreflightInputs in = Good();
        in.hooksOk = false;
        CHECK(policy.Evaluate(in, kNow).reason == Fail::HooksInconsistent);

        PreflightInputs both = Good();
        both.active = Target::Plain;
        both.target = Target::Plain;
        both.hooksOk = false;
        CHECK(policy.Evaluate(both, kNow).kind == VerdictKind::Idle);

        PreflightInputs fromPlain = Good();
        fromPlain.active = Target::Plain;
        fromPlain.target = Target::FsrFg;
        fromPlain.hooksOk = false;
        CHECK(policy.Evaluate(fromPlain, kNow).reason == Fail::HooksInconsistent);
    }
    std::printf("    [ok] a plain move needs neither a model nor hooks; a provider class needs both\n");
}

void OrderOfReasons()
{
    std::printf("  the reported reason is the most basic cause\n");
    Policy policy;
    PreflightInputs in = Good();
    in.bridgePresent = false;
    in.loadersOk = false;
    in.canCapture = false;
    CHECK(policy.Evaluate(in, kNow).reason == Fail::NoBridge);

    Policy second;
    PreflightInputs both = Good();
    both.gameHdr = true;
    both.shuttingDown = true;
    CHECK(second.Evaluate(both, kNow).reason == Fail::ShuttingDown);
    std::printf("    [ok] no bridge before no provider, shutting down before HDR\n");
}

void TransientBackoffAndGiveUp()
{
    std::printf("  a transient cause backs off, and ten refusals in a row are remembered\n");
    static_assert(kTransientRetryMaxMs == 30000, "the pause never grows past thirty seconds");
    static_assert(kTransientGiveUp == 10, "ten refusals in a row are two minutes of backoff");
    {
        Policy policy;
        uint64_t now = kNow;
        const uint64_t pauses[] = { 1000, 2000, 4000, 8000, 16000, 30000, 30000, 30000, 30000, 30000 };
        for (uint32_t i = 0; i < kTransientGiveUp; ++i)
        {
            policy.PauseTransient(now, Target::XeFg, Fail::Drain);
            CHECK(policy.RetryAfterMs() == now + pauses[i]);
            if (i + 1 < kTransientGiveUp)
                CHECK(!policy.Remembered(Target::XeFg)); // a cause that may pass is not a property of the target
            now += pauses[i];
        }
        CHECK(policy.TransientStreak() == kTransientGiveUp);
        CHECK(policy.Remembered(Target::XeFg));
        CHECK(policy.RememberedReason(Target::XeFg) == Fail::Drain);
    }
    {
        // A commit starts the count over. A Go does not: an attempt that reaches Go and then refuses inside the sequence
        // is the next refusal in a row, which is exactly the case the backoff exists for.
        Policy policy;
        PreflightInputs in = Good();
        policy.PauseTransient(kNow, in.target, Fail::Drain);
        CHECK(policy.TransientStreak() == 1);
        CHECK(policy.Evaluate(in, kNow + kTransientRetryMs).kind == VerdictKind::Go);
        CHECK(policy.TransientStreak() == 1);
        policy.PauseTransient(kNow + 2000, in.target, Fail::Drain);
        policy.Committed(kNow + 3000);
        CHECK(policy.TransientStreak() == 0);
    }
    std::printf("    [ok] 1, 2, 4, 8, 16 and 30 s; the tenth stores the cause and the menu can say restart\n");
}

void WitnessUsable()
{
    std::printf("  a row that needs the witness refuses when it cannot be placed\n");
    Policy policy;
    PreflightInputs in = Good();
    in.witnessUsable = false;
    const Verdict refused = policy.Evaluate(in, kNow);
    CHECK(refused.kind == VerdictKind::Refuse);
    CHECK(refused.reason == Fail::WitnessUnavailable);
    CHECK(refused.severity == Severity::Hard);
    CHECK(policy.Remembered(in.target));

    // A row measured without one (M1 without a witness) does not ask for it: the more specific input wins.
    Policy plain;
    PreflightInputs in2 = Good();
    in2.witnessUsable = false;
    in2.activeModel.witnessHonoured = false;
    in2.targetModel.witnessHonoured = false;
    CHECK(plain.Evaluate(in2, kNow).kind == VerdictKind::Go);

    // Only the row in play asks for it: a witness-less active class does not make the target's refusal disappear.
    Policy mixed;
    PreflightInputs in3 = Good();
    in3.witnessUsable = false;
    in3.activeModel.witnessHonoured = false;
    in3.targetModel.witnessHonoured = true;
    CHECK(mixed.Evaluate(in3, kNow).reason == Fail::WitnessUnavailable);
    std::printf("    [ok] the refusal is hard and only raised for a row that carries witness=honoured\n");
}

void InSequenceRefusalsAndTheRefCount()
{
    std::printf("  a transient refusal raised inside the sequence takes the same pause\n");
    {
        Policy policy;
        PreflightInputs in = Good();
        CHECK(policy.Evaluate(in, kNow).kind == VerdictKind::Go);
        // The sequence hit a barrier timeout: PauseTransient is what the core calls for it.
        policy.PauseTransient(kNow, in.target, Fail::Barrier);
        CHECK(policy.Evaluate(in, kNow).kind == VerdictKind::Idle);
        CHECK(policy.Evaluate(in, kNow + kTransientRetryMs - 1).kind == VerdictKind::Idle);
        CHECK(policy.Evaluate(in, kNow + kTransientRetryMs).kind == VerdictKind::Go);
        CHECK(!policy.Remembered(in.target));
    }
    std::printf("    [ok] the pause is a second long and remembers nothing\n");

    std::printf("  a transient cause is never remembered, a hard one is\n");
    {
        Policy policy;
        policy.Remember(Target::XeFg, Fail::HookBusy);
        CHECK(!policy.Remembered(Target::XeFg));
        policy.Remember(Target::XeFg, Fail::Drain);
        CHECK(!policy.Remembered(Target::XeFg));
        policy.Remember(Target::XeFg, Fail::HookFailed);
        CHECK(policy.Remembered(Target::XeFg));
        CHECK(policy.RememberedReason(Target::XeFg) == Fail::HookFailed);
        // A transient cause after a hard one does not overwrite the stored cause either.
        policy.Remember(Target::XeFg, Fail::HookBusy);
        CHECK(policy.RememberedReason(Target::XeFg) == Fail::HookFailed);
    }
    std::printf("    [ok] Remember ignores transient causes and keeps the first hard one\n");

    std::printf("  a chain held by more references than the first present measured is refused\n");
    {
        Policy policy;
        PreflightInputs in = Good();
        in.activeRefs = 3;
        in.expectedRefs = 3;
        CHECK(policy.Evaluate(in, kNow).kind == VerdictKind::Go);
        // One more holder (an overlay, a recorder, a module that grabbed the chain): transient, nothing touched.
        in.activeRefs = 4;
        const Verdict mismatch = policy.Evaluate(in, kNow + kTransientRetryMs);
        CHECK(mismatch.reason == Fail::RefMismatch);
        CHECK(mismatch.severity == Severity::Transient);
        CHECK(!policy.Remembered(in.target));
        // The holder let go: the switch goes ahead.
        in.activeRefs = 3;
        CHECK(policy.Evaluate(in, kNow + 2 * kTransientRetryMs).kind == VerdictKind::Go);
        // Not measured on either side is not a mismatch. A fresh policy, because the clock here is not monotonic across
        // the two blocks and a pause from the previous refusal would hide the answer.
        Policy fresh;
        PreflightInputs unmeasured = Good();
        unmeasured.activeRefs = 5;
        CHECK(fresh.Evaluate(unmeasured, kNow).kind == VerdictKind::Go);
    }
    std::printf("    [ok] RefMismatch is transient, per attempt, and only when both counts are known\n");
}

void StatusAndTexts()
{
    std::printf("  the texts the menu and the log show\n");
    CHECK(std::string_view(TargetText(Target::Plain)) == "none");
    CHECK(std::string_view(TargetText(Target::FsrFg)) == "fsrfg");
    CHECK(std::string_view(TargetText(Target::XeFg)) == "xefg");
    CHECK(std::string_view(ClassText(Class::Plain)) == "plain");
    CHECK(std::string_view(ModelText(Model::Unknown)) == "unknown");
    CHECK(std::string_view(ModelText(Model::M1)) == "M1");
    CHECK(std::string_view(ModelText(Model::M2)) == "M2");
    CHECK(std::string_view(StateText(State::Dead)) == "restart needed");
    CHECK(std::string_view(FailText(Fail::NoModel)).find("measured") != std::string_view::npos);
    CHECK(std::string_view(SeverityText(Severity::Transient)) == "transient");

    Status status;
    CHECK(status.state == State::Idle);
    CHECK(status.reason == Fail::None);
    CHECK(status.switchesLeft == 0);
    std::printf("    [ok] every name has its text, the status starts idle\n");

    // The constants the design fixes; a test that lets them drift silently would let the behaviour drift too.
    static_assert(kMaxSwitches == 8, "the session switch budget is eight");
    static_assert(kCooldownMs == 3000, "the cooldown after a commit is three seconds");
    static_assert(kTransientRetryMs == 1000, "a transient refusal is retried after a second");
    static_assert(kResizeBusyMs == 500, "a resize within half a second refuses the switch");
    static_assert(kThreadMixMs == 5000, "a foreign-thread resize within five seconds refuses the switch");
    std::printf("    [ok] budget 8, cooldown 3 s, transient retry 1 s, resize 500 ms, thread mix 5 s\n");
}
// The XeFG direction table: each of the four directions is its own switch, and the two FSR FG <-> plain moves never ask.
void XeFgDirectionTable()
{
    std::printf("  the XeFG directions are allowed one by one\n");
    const XeFgDirections all {};
    CHECK(XeFgDirectionAllowed(all, Target::FsrFg, Target::XeFg));
    CHECK(XeFgDirectionAllowed(all, Target::Plain, Target::XeFg));
    CHECK(XeFgDirectionAllowed(all, Target::XeFg, Target::FsrFg));
    CHECK(XeFgDirectionAllowed(all, Target::XeFg, Target::Plain));

    struct Case
    {
        XeFgDirections table;
        Target active;
        Target target;
    };
    XeFgDirections noFsrToXe;
    noFsrToXe.fsrToXe = false;
    XeFgDirections noPlainToXe;
    noPlainToXe.plainToXe = false;
    XeFgDirections noXeToFsr;
    noXeToFsr.xeToFsr = false;
    XeFgDirections noXeToPlain;
    noXeToPlain.xeToPlain = false;
    const Case refused[] = {
        { noFsrToXe, Target::FsrFg, Target::XeFg },
        { noPlainToXe, Target::Plain, Target::XeFg },
        { noXeToFsr, Target::XeFg, Target::FsrFg },
        { noXeToPlain, Target::XeFg, Target::Plain },
    };
    for (const Case& c : refused)
    {
        CHECK(!XeFgDirectionAllowed(c.table, c.active, c.target));
        // Only that direction: the other three still pass with the same table.
        int allowed = 0;
        const Target pairs[4][2] = { { Target::FsrFg, Target::XeFg }, { Target::Plain, Target::XeFg },
                                     { Target::XeFg, Target::FsrFg }, { Target::XeFg, Target::Plain } };
        for (const auto& pair : pairs)
            allowed += XeFgDirectionAllowed(c.table, pair[0], pair[1]) ? 1 : 0;
        CHECK(allowed == 3);
        // FSR FG <-> plain is not an XeFG direction, whatever the table says.
        CHECK(XeFgDirectionAllowed(c.table, Target::FsrFg, Target::Plain));
        CHECK(XeFgDirectionAllowed(c.table, Target::Plain, Target::FsrFg));
    }

    // Through the policy: a refused direction is a hard XeFgSwitch refusal, an allowed one is not refused for it.
    PreflightInputs inputs = Good();
    inputs.active = Target::XeFg;
    inputs.target = Target::Plain;
    inputs.xeFgSwitchAllowed = XeFgDirectionAllowed(noXeToPlain, inputs.active, inputs.target);
    Policy policy;
    const Verdict verdict = policy.Evaluate(inputs, 1000);
    CHECK(verdict.kind == VerdictKind::Refuse && verdict.reason == Fail::XeFgSwitch);
    inputs.target = Target::FsrFg;
    inputs.xeFgSwitchAllowed = XeFgDirectionAllowed(noXeToPlain, inputs.active, inputs.target);
    Policy other;
    CHECK(other.Evaluate(inputs, 1000).reason != Fail::XeFgSwitch);
    std::printf("    [ok] each XeFG direction is refused alone, FSR FG <-> plain is never asked\n");
}
} // namespace

int main()
{
    Verdicts();
    XeFgDirectionTable();
    Idle();
    TransientPause();
    Memory();
    CooldownAndBudget();
    FirstCauseAndSessionText();
    ModelAndHooksRules();
    InSequenceRefusalsAndTheRefCount();
    TransientBackoffAndGiveUp();
    WitnessUsable();
    OrderOfReasons();
    StatusAndTexts();

    if (g_failed == 0)
        std::printf("ALL POLICY SCENARIOS PASS\n");
    else
        std::printf("%d CHECK(S) FAILED\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
