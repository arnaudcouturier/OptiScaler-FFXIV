#pragma once
// Live frame-generation output switch: the vocabulary the pure core and its runtime binding share.
//
// The switch swaps the swapchain that carries frame generation while the game runs: FSR FG, XeFG or plain presentation.
// Everything that can be decided without Direct3D lives in this directory and is unit-tested with fakes
// (tests/ext/fg/fg_switch_*). This header has no Windows, DXGI or upstream include on purpose: the same code runs in the
// product and in the standalone tests.
//
// Two numbers are used throughout instead of HRESULT/DXGI types: `uint32_t` result codes, compared with Succeeded()
// and Failed(). The runtime casts them; the tests state them literally.

#include <cstdint>
#include <cstdio>

namespace ext::fg::outswitch
{
// Result codes the core reasons about. Identical values to the DXGI/Win32 ones the runtime passes in.
inline constexpr uint32_t kOk = 0x00000000u;
inline constexpr uint32_t kOccluded = 0x087A0001u;     // DXGI_STATUS_OCCLUDED: the window is not visible, not a failure
inline constexpr uint32_t kClipped = 0x087A0002u;      // DXGI_STATUS_CLIPPED: presented to a clipped region
inline constexpr uint32_t kNoRedirection = 0x087A0004u; // DXGI_STATUS_NO_REDIRECTION
inline constexpr uint32_t kModeChanged = 0x087A0007u;  // DXGI_STATUS_MODE_CHANGED
inline constexpr uint32_t kModeChangeInProgress = 0x087A0008u;
inline constexpr uint32_t kAccessDenied = 0x80070005u; // E_ACCESSDENIED: the window is still bound to a swapchain
inline constexpr uint32_t kDeviceHung = 0x887A0006u;
inline constexpr uint32_t kDeviceReset = 0x887A0007u;
inline constexpr uint32_t kDeviceRemoved = 0x887A0005u;
// DXGI_ERROR_WAS_STILL_DRAWING: not a chain failure. A DXGI_PRESENT_DO_NOT_WAIT present returns it when the frame it was
// asked about is still in flight (REFERENCES.md [MS-3], the DXGI_PRESENT flags); the chain is fine and will present the
// next frame. Probation must not roll a healthy chain back because a game presented without waiting.
inline constexpr uint32_t kStillDrawing = 0x887A000Au;

constexpr bool Succeeded(uint32_t result) { return static_cast<int32_t>(result) >= 0; }
constexpr bool Failed(uint32_t result) { return !Succeeded(result); }

// A present result that says nothing bad about the chain: a success code (S_OK and, being status codes, every
// DXGI_STATUS_* such as DXGI_STATUS_OCCLUDED or DXGI_STATUS_CLIPPED) or DXGI_ERROR_WAS_STILL_DRAWING above.
constexpr bool PresentHealthy(uint32_t result) { return Succeeded(result) || result == kStillDrawing; }

// The device is gone: the game has to handle that itself, so the result is never replaced with S_OK.
constexpr bool DeviceLost(uint32_t result)
{
    return result == kDeviceRemoved || result == kDeviceHung || result == kDeviceReset;
}

// A present the chain accepted for a probe (a test present, not a real frame).
constexpr bool PresentAccepted(uint32_t result) { return result == kOk || result == kOccluded; }

// The output the switch is asked to put on the window. `Plain` is the bridge's own DX12 flip chain with no frame
// generation, which is what a rollback restores.
enum class Target : uint8_t
{
    Plain,
    FsrFg,
    XeFg
};

// The swapchain class behind a live chain. One chain exists at a time, so one class is hooked at a time.
enum class Class : uint8_t
{
    Plain,
    FsrFg,
    XeFg
};

constexpr Class ClassOf(Target target)
{
    switch (target)
    {
    case Target::FsrFg:
        return Class::FsrFg;
    case Target::XeFg:
        return Class::XeFg;
    default:
        return Class::Plain;
    }
}

constexpr bool IsProvider(Class cls) { return cls != Class::Plain; }

// What a frame-generation library's destroy call does to the swapchain object. Measured per class and per library
// build by the probe (LibraryModel.h): the code alone cannot decide, because both upstream release paths exist.
//   M1  the destroy ends the object whatever the reference count says: a later release touches freed memory
//   M2  the context holds one reference and the destroy drops it: the caller's release then frees the object
enum class Model : uint8_t
{
    Unknown,
    M1,
    M2
};

// One row of the measured model table: the model plus whether the library honours a private-data interface on the
// outer chain (the diagnostic cross-check; it never decides a release on its own).
struct LibraryModel
{
    Model model = Model::Unknown;
    bool witnessHonoured = false;
    // The order the row was measured with: the caller's reference released first, then the library's destroy. A row is
    // only used for its own order (LibraryModel.h), because the two orders free the same object differently.
    bool pinFirst = false;
    uint32_t standstillRefs = 0; // the count the row measured at standstill (0 = not measured)
};

// Why a switch did not run, or stopped. The texts are player-facing (menu status line) and log-facing.
enum class Fail : uint8_t
{
    None,
    KeyOff,   // the feature key is off: idle, nothing logged
    AlreadyActive, // the configured output is the active one: idle
    Unsupported, // DLSS-G, reprojection or a value the switch does not know
    XeFgSwitch, // XeFG is on the window or is the target, in a direction this build does not take live
    NoBridge, // the game is not on the DX11->DX12 bridge
    ShuttingDown,
    Fullscreen,
    WaitableCached, // the chain has the frame-latency waitable flag: a cached handle would go stale
    GameHdr, // game-native HDR is not replayed
    HdrFsr, // the fork's own HDR output is on and FSR FG cannot run under it
    MissingProvider, // the target's libraries do not load
    NoModel, // the measured model table has no row for the target's library build
    NoCapture, // descriptor, factory or queue of the live chain could not be read
    RefMismatch, // the live chain carries a reference nobody accounted for since the first present measured it
    Remembered, // this target failed before; restart or "try again"
    Cooling, // a commit happened less than a moment ago
    Budget, // the session switch budget is spent
    ResizeBusy, // a resize ran less than a moment ago
    ThreadMix, // a resize came from another thread a moment ago
    HooksInconsistent, // the hook slots are in a state the rebind cannot start from
    SecondBridge, // the process created a second bridge: one window, one chain
    Barrier, // no proof that no native present was inside the resize mutex
    Drain, // a queue did not prove idle
    TeardownRefused, // the provider refused to release the chain it was asked about
    ModelViolation, // the witness disagrees with the measured model: stop touching the chain
    WitnessUnavailable, // the row says the witness is honoured, but the live chain refused the interface
    OldChainLeaked, // the window is still bound to a chain that could not be freed
    Create, // the target's chain could not be created
    CreateDenied, // every create attempt was refused because the window is still bound
    HookFailed, // the old class's hooks could not be detached: the target is not at fault, the source class stays patched
    HookBusy, // a hook body did not leave within the drain timeout: the old class stays patched, nothing is remembered
    Rehook, // the new chain was created but no Present hook reached it
    Adopt, // the new chain could not be handed to the bridge
    Replay, // colour space, HDR or latency could not be restored on the new chain
    TestPresent, // the new chain answered the test present with a failure
    Probation, // the new chain failed a present after it was committed
    DeviceLost, // the device was lost: the game handles it, no rollback
    Stall, // one present took too long during probation
    PlainCreate, // no chain could be put on the window at all
    Dead // the module gave up for this session
};

// What a hook detach answered. The three answers are three different decisions:
//   Ok      the originals are gone (or nothing was attached): the new class can be hooked
//   Busy    a hook body did not leave within the drain timeout (HookLifecycle.h). Nothing is remembered: the cause is a
//           call in flight, not a property of the target, and the old class stays patched as dead code meanwhile
//   Failed  the slots could not be detached at all (an inconsistent set, a foreign detour): remembered, restart needed
enum class UnhookResult : uint8_t
{
    Ok,
    Busy,
    Failed
};

// How a refusal is remembered. Hard refusals are per target until restart (menu: restart / try again); transient ones
// are retried after a moment because nothing was touched and the cause can pass (a busy GPU, a resize, a lost focus).
enum class Severity : uint8_t
{
    None,
    Transient,
    Hard
};

constexpr Severity SeverityOf(Fail fail)
{
    switch (fail)
    {
    case Fail::None:
    case Fail::KeyOff:
    case Fail::AlreadyActive:
        return Severity::None;
    case Fail::ShuttingDown:
    case Fail::Fullscreen:
    case Fail::Cooling:
    case Fail::ResizeBusy:
    case Fail::ThreadMix:
    case Fail::Barrier:
    case Fail::Drain:
    case Fail::RefMismatch:
    case Fail::HookBusy:
        return Severity::Transient;
    default:
        return Severity::Hard;
    }
}

// The state the menu and status.json show (atomics in the runtime, plain data here).
enum class State : uint8_t
{
    Idle,
    Switching,
    Probation,
    Refused,
    RolledBack,
    Limp,
    Dead
};

struct Status
{
    State state = State::Idle;
    Fail reason = Fail::None;
    Target target = Target::Plain; // the configured output the switch was asked for
    Target active = Target::Plain; // the output that is on the window now
    uint32_t generation = 0;
    uint32_t lastHitchMs = 0;
    uint32_t switchesLeft = 0;
    uint32_t probationPresents = 0;
};

// Two status values say the same thing. The menu thread publishes only a change, so a present that refuses the same
// target for the same cause does not write a line and a status file sixty times a second.
constexpr bool SameStatus(const Status& a, const Status& b)
{
    return a.state == b.state && a.reason == b.reason && a.target == b.target && a.active == b.active &&
           a.generation == b.generation && a.switchesLeft == b.switchesLeft && a.probationPresents == b.probationPresents &&
           a.lastHitchMs == b.lastHitchMs;
}

// The step a log line belongs to. The runtime prints them with one prefix; the tests assert the order.
enum class Step : uint8_t
{
    Idle,
    Begin,
    Preflight, // the verdict was Go; nothing has been touched yet
    Refused, // no, with the reason and its class
    Barrier,
    Quiesce,
    Drain,
    Capture,
    Detach,
    Teardown,
    Unhook,
    Create,
    Rehook,
    Adopt,
    Replay,
    TestPresent,
    Commit,
    Rollback,
    Probation,
    Limp,
    Dead,
    Detached // a present that arrived while the bridge had no chain
};

// One observable step. The runtime formats and prints it; the tests record the sequence. `a`, `b`, `c` carry the
// numbers that step logs (retries, result codes, counts), `flagA`/`flagB` the yes/no pairs (pin release, drained).
struct Event
{
    Step step = Step::Idle;
    Fail fail = Fail::None;
    Severity severity = Severity::None;
    Target from = Target::Plain;
    Target to = Target::Plain;
    Class cls = Class::Plain;
    Model model = Model::Unknown;
    uint32_t generation = 0;
    uint32_t a = 0;
    uint32_t b = 0;
    uint32_t c = 0;
    int64_t ms = 0;
    bool flagA = false;
    bool flagB = false;
    // The chain a create or teardown step is about, for the one line that names it. Never dereferenced by the core.
    const void* chain = nullptr;
    const char* text = "";
};

constexpr const char* TargetText(Target target)
{
    switch (target)
    {
    case Target::FsrFg:
        return "fsrfg";
    case Target::XeFg:
        return "xefg";
    default:
        return "none";
    }
}

constexpr const char* ClassText(Class cls)
{
    switch (cls)
    {
    case Class::FsrFg:
        return "fsrfg";
    case Class::XeFg:
        return "xefg";
    default:
        return "plain";
    }
}

constexpr const char* ModelText(Model model)
{
    switch (model)
    {
    case Model::M1:
        return "M1";
    case Model::M2:
        return "M2";
    default:
        return "unknown";
    }
}

constexpr const char* SeverityText(Severity severity)
{
    switch (severity)
    {
    case Severity::Hard:
        return "hard";
    case Severity::Transient:
        return "transient";
    default:
        return "none";
    }
}

constexpr const char* StateText(State state)
{
    switch (state)
    {
    case State::Switching:
        return "switching";
    case State::Probation:
        return "probation";
    case State::Refused:
        return "refused";
    case State::RolledBack:
        return "rolled back";
    case State::Limp:
        return "limping";
    case State::Dead:
        return "restart needed";
    default:
        return "idle";
    }
}

// A short reason for the log and the menu. The menu adds its own sentence; the log prints this verbatim.
constexpr const char* FailText(Fail fail)
{
    switch (fail)
    {
    case Fail::KeyOff:
        return "the feature key is off";
    case Fail::AlreadyActive:
        return "already the active output";
    case Fail::Unsupported:
        return "the configured output cannot be switched to";
    case Fail::XeFgSwitch:
        return "this XeFG direction is not switched live in this build";
    case Fail::NoBridge:
        return "the game is not on the DX11 to DX12 bridge";
    case Fail::ShuttingDown:
        return "the process is shutting down";
    case Fail::Fullscreen:
        return "the window is exclusive fullscreen";
    case Fail::WaitableCached:
        return "the chain has a frame latency waitable object";
    case Fail::GameHdr:
        return "the game set game-native HDR";
    case Fail::HdrFsr:
        return "the fork's HDR output is on and FSR FG cannot run under it";
    case Fail::MissingProvider:
        return "the target's libraries do not load";
    case Fail::NoModel:
        return "this library build has no measured release model";
    case Fail::NoCapture:
        return "the live chain could not be read";
    case Fail::RefMismatch:
        return "the live chain is held by someone else than at the first present";
    case Fail::Remembered:
        return "this target failed earlier in this session";
    case Fail::Cooling:
        return "a switch just happened";
    case Fail::Budget:
        return "the session switch budget is spent";
    case Fail::ResizeBusy:
        return "a resize just happened";
    case Fail::ThreadMix:
        return "a resize came from another thread";
    case Fail::HooksInconsistent:
        return "the swapchain hooks are in an unexpected state";
    case Fail::SecondBridge:
        return "the game created a second swapchain bridge";
    case Fail::Barrier:
        return "a native present did not finish in time";
    case Fail::Drain:
        return "a queue did not prove idle";
    case Fail::TeardownRefused:
        return "the provider refused to release its chain";
    case Fail::ModelViolation:
        return "the release witness disagrees with the measured model";
    case Fail::WitnessUnavailable:
        return "the release witness could not be placed on the live chain";
    case Fail::OldChainLeaked:
        return "the window is still bound to the old chain";
    case Fail::Create:
        return "the new chain could not be created";
    case Fail::CreateDenied:
        return "the window stayed bound to the old chain";
    case Fail::HookFailed:
        return "the old chain class could not be unhooked";
    case Fail::Rehook:
        return "the new chain class could not be hooked";
    case Fail::HookBusy:
        return "a hook call did not finish in time";
    case Fail::Adopt:
        return "the new chain could not be handed to the bridge";
    case Fail::Replay:
        return "the chain settings could not be restored";
    case Fail::TestPresent:
        return "the new chain failed its test present";
    case Fail::Probation:
        return "the new chain failed a present";
    case Fail::DeviceLost:
        return "the device was lost";
    case Fail::Stall:
        return "one present took too long";
    case Fail::PlainCreate:
        return "no chain could be put on the window";
    case Fail::Dead:
        return "the module gave up for this session";
    default:
        return "";
    }
}

// The menu's sentence for a refusal the memory keeps: hard refusals are never retried automatically, and no input
// changing later clears that memory. Only a restart or the menu's own "try again" does.
inline void SessionRefusedText(const Status& status, char (&out)[224])
{
    const char* reason = status.reason != Fail::None ? FailText(status.reason) : "the switch was refused";
    std::snprintf(out, sizeof(out), "refused for this session: %s; restart the game to try again", reason);
}
} // namespace ext::fg::outswitch
