#pragma once
// The `[fg-switch]` line of one step: what the log shows, what the benchmark driver parses and what the event recorder
// stores. One line per step, its fields in a fixed order, everything on that one line so a reader can pick it out with
// the prefix and a tool can split it on spaces:
//
//   begin gen=4 from=fsrfg to=xefg why=script
//   preflight ok loaders=fsrfg,xefg thread=14404 resizeGen=0
//   refused reason=Fullscreen class=transient target=xefg
//   barrier ok waitedMs=2
//   drain game=Drained fg=Drained ms=117
//   teardown old chain=000001C4A2B3D4E0 class=xefg lib=libxess_fg.dll/12345678/65F1A2B3 model=M2 pinRelease=yes witness=alive ms=812
//   hooks unhooked=7 raw=3
//   create ok chain=000001C4A2B3D4F8 hr=0x0 retries=0 ms=1543
//   hooks class vtbl[8]=00007FFB2A3C1120
//   adopt ok colorspace=0 hdr=off
//   commit gen=4 hitchMs=812 budgetLeft=5
//   probation ok presents=20 ms=8300 output=fg
//   FAILED step=create hr=0x80070005 rollback=plain remember=xefg
//
// A reason, a model and a target are tokens (the enum's name), never a sentence: the menu prints the sentence.
//
// Pure: no Windows, DXGI or upstream include (tests/ext/fg/fg_switch_log.cpp). The product fills the fields it reads
// from the bridge, the provider adapters and State, and calls FormatSwitchLine once per step; the same text is what the
// recorder's switch tap stores, so the log and the recording never disagree.

#include "ext/fg/outswitch/SwitchTypes.h"

#include <cstdint>
#include <cstdio>

namespace ext::fg::outswitch
{
// One step's fields. Every field is optional: a step prints only the ones its line names, so a wrong value never leaks
// from another step's line.
struct SwitchLogFields
{
    Step step = Step::Idle;
    Fail fail = Fail::None;
    Severity severity = Severity::None;
    Target from = Target::Plain;
    Target to = Target::Plain;
    Class cls = Class::Plain;
    Model model = Model::Unknown;
    uint32_t generation = 0;
    uint32_t budgetLeft = 0;
    uint32_t thread = 0;
    uint32_t resizeGen = 0;
    bool loadersFsr = false;
    bool loadersXe = false;
    uintptr_t chain = 0; // the created or torn-down chain, printed as 16 hex digits
    uintptr_t vtbl = 0;  // the new class's Present slot
    char libraryFile[64] {};
    uint64_t librarySize = 0;
    uint32_t libraryStamp = 0;
    bool pinReleased = false;
    bool witnessAlive = false;
    bool witnessPlaced = true; // false: no witness was placed on this chain, the line says witness=na
    uint32_t unhooked = 0;
    uint32_t raw = 0;
    uint32_t retries = 0;
    uint32_t hr = 0;
    int64_t ms = 0;
    bool gameDrained = true;
    bool fgDrained = true;
    uint32_t colorspace = 0;
    bool hdr = false;
    uint32_t presents = 0;
    bool fgActive = false;
    bool ok = true;
    bool rebuild = false; // the limp may still rebuild a plain chain
    const char* why = "config";
};

// The name a step without a line of its own prints (quiesce, capture, detach, replay): never "idle", which would say
// the present did nothing while it was mid-switch.
inline const char* StepName(Step step)
{
    switch (step)
    {
    case Step::Begin:
        return "begin";
    case Step::Preflight:
        return "preflight";
    case Step::Refused:
        return "refused";
    case Step::Barrier:
        return "barrier";
    case Step::Quiesce:
        return "quiesce";
    case Step::Drain:
        return "drain";
    case Step::Capture:
        return "capture";
    case Step::Detach:
        return "detach";
    case Step::Teardown:
        return "teardown";
    case Step::Unhook:
        return "hooks";
    case Step::Create:
        return "create";
    case Step::Rehook:
        return "rehook";
    case Step::Adopt:
        return "adopt";
    case Step::Replay:
        return "replay";
    case Step::TestPresent:
        return "test-present";
    case Step::Commit:
        return "commit";
    case Step::Rollback:
        return "rollback";
    case Step::Probation:
        return "probation";
    case Step::Limp:
        return "limp";
    case Step::Dead:
        return "dead";
    case Step::Detached:
        return "detached";
    default:
        return "idle";
    }
}

// The token a reason prints as. A closed set: the menu's FailText(Fail) is the sentence, this is what a tool reads.
inline const char* FailName(Fail fail)
{
    switch (fail)
    {
    case Fail::KeyOff:
        return "KeyOff";
    case Fail::AlreadyActive:
        return "AlreadyActive";
    case Fail::Unsupported:
        return "Unsupported";
    case Fail::XeFgSwitch:
        return "XeFgSwitch";
    case Fail::NoBridge:
        return "NoBridge";
    case Fail::ShuttingDown:
        return "ShuttingDown";
    case Fail::Fullscreen:
        return "Fullscreen";
    case Fail::WaitableCached:
        return "WaitableCached";
    case Fail::GameHdr:
        return "GameHdr";
    case Fail::HdrFsr:
        return "HdrFsr";
    case Fail::MissingProvider:
        return "MissingProvider";
    case Fail::NoModel:
        return "NoModel";
    case Fail::NoCapture:
        return "NoCapture";
    case Fail::RefMismatch:
        return "RefMismatch";
    case Fail::Remembered:
        return "Remembered";
    case Fail::Cooling:
        return "Cooling";
    case Fail::Budget:
        return "Budget";
    case Fail::ResizeBusy:
        return "ResizeBusy";
    case Fail::ThreadMix:
        return "ThreadMix";
    case Fail::HooksInconsistent:
        return "HooksInconsistent";
    case Fail::SecondBridge:
        return "SecondBridge";
    case Fail::Barrier:
        return "Barrier";
    case Fail::Drain:
        return "Drain";
    case Fail::TeardownRefused:
        return "TeardownRefused";
    case Fail::ModelViolation:
        return "ModelViolation";
    case Fail::WitnessUnavailable:
        return "WitnessUnavailable";
    case Fail::OldChainLeaked:
        return "OldChainLeaked";
    case Fail::Create:
        return "Create";
    case Fail::CreateDenied:
        return "CreateDenied";
    case Fail::HookFailed:
        return "HookFailed";
    case Fail::HookBusy:
        return "HookBusy";
    case Fail::Rehook:
        return "Rehook";
    case Fail::Adopt:
        return "Adopt";
    case Fail::Replay:
        return "Replay";
    case Fail::TestPresent:
        return "TestPresent";
    case Fail::Probation:
        return "Probation";
    case Fail::DeviceLost:
        return "DeviceLost";
    case Fail::Stall:
        return "Stall";
    case Fail::PlainCreate:
        return "PlainCreate";
    case Fail::Dead:
        return "Dead";
    default:
        return "none";
    }
}

// The step a failure belongs to, for the one `FAILED step=` line a rollback prints.
inline const char* FailStepName(Fail fail)
{
    switch (fail)
    {
    case Fail::NoCapture:
        return "capture";
    case Fail::Barrier:
        return "barrier";
    case Fail::Drain:
        return "drain";
    case Fail::TeardownRefused:
    case Fail::ModelViolation:
    case Fail::WitnessUnavailable:
    case Fail::OldChainLeaked:
        return "teardown";
    case Fail::HookFailed:
    case Fail::HookBusy:
    case Fail::HooksInconsistent:
        return "hooks";
    case Fail::Create:
    case Fail::CreateDenied:
        return "create";
    case Fail::Rehook:
        return "rehook";
    case Fail::Adopt:
        return "adopt";
    case Fail::Replay:
        return "replay";
    case Fail::TestPresent:
        return "test-present";
    case Fail::Probation:
    case Fail::Stall:
        return "probation";
    case Fail::DeviceLost:
        return "device";
    case Fail::PlainCreate:
        return "plain-create";
    default:
        return "preflight";
    }
}

// "fsrfg,xefg", "xefg", "fsrfg" or "none": which of the two providers this process loaded, once per preflight.
inline const char* LoadersName(const SwitchLogFields& f, char (&out)[32])
{
    if (f.loadersFsr && f.loadersXe)
        std::snprintf(out, sizeof(out), "fsrfg,xefg");
    else if (f.loadersFsr)
        std::snprintf(out, sizeof(out), "fsrfg");
    else if (f.loadersXe)
        std::snprintf(out, sizeof(out), "xefg");
    else
        std::snprintf(out, sizeof(out), "none");
    return out;
}

// The body of one line, without the `[fg-switch] ` prefix (the caller adds it once). Never reads anything but `f`.
inline void FormatSwitchLine(const SwitchLogFields& f, char (&out)[512])
{
    switch (f.step)
    {
    case Step::Begin:
        std::snprintf(out, sizeof(out), "begin gen=%u from=%s to=%s why=%s", f.generation, TargetText(f.from),
                      TargetText(f.to), f.why != nullptr ? f.why : "config");
        break;
    case Step::Preflight:
    {
        char loaders[32];
        std::snprintf(out, sizeof(out), "preflight ok loaders=%s thread=%u resizeGen=%u", LoadersName(f, loaders),
                      f.thread, f.resizeGen);
        break;
    }
    case Step::Refused:
        std::snprintf(out, sizeof(out), "refused reason=%s class=%s target=%s", FailName(f.fail),
                      SeverityText(f.severity), TargetText(f.to));
        break;
    case Step::Barrier:
        std::snprintf(out, sizeof(out), "barrier %s waitedMs=%lld", f.ok ? "ok" : "failed",
                      static_cast<long long>(f.ms));
        break;
    case Step::Drain:
        std::snprintf(out, sizeof(out), "drain game=%s fg=%s ms=%lld", f.gameDrained ? "Drained" : "busy",
                      f.fgDrained ? "Drained" : "busy", static_cast<long long>(f.ms));
        break;
    case Step::Teardown:
    {
        char library[128];
        if (f.libraryFile[0] != '\0')
            std::snprintf(library, sizeof(library), "%s/%llu/%08X", f.libraryFile,
                          static_cast<unsigned long long>(f.librarySize), f.libraryStamp);
        else
            std::snprintf(library, sizeof(library), "none");
        const char* witness = f.model == Model::M1 || !f.witnessPlaced ? "na" : (f.witnessAlive ? "alive" : "dead");
        std::snprintf(out, sizeof(out),
                      "teardown old chain=%016llX class=%s lib=%s model=%s pinRelease=%s witness=%s ms=%lld",
                      static_cast<unsigned long long>(f.chain), ClassText(f.cls), library, ModelText(f.model),
                      f.pinReleased ? "yes" : "no", witness, static_cast<long long>(f.ms));
        break;
    }
    case Step::Unhook:
        std::snprintf(out, sizeof(out), "hooks unhooked=%u raw=%u", f.unhooked, f.raw);
        break;
    case Step::Create:
        std::snprintf(out, sizeof(out), "create %s chain=%016llX hr=0x%08X retries=%u ms=%lld", f.ok ? "ok" : "failed",
                      static_cast<unsigned long long>(f.chain), f.hr, f.retries, static_cast<long long>(f.ms));
        break;
    case Step::Rehook:
        std::snprintf(out, sizeof(out), "hooks class vtbl[8]=%016llX", static_cast<unsigned long long>(f.vtbl));
        break;
    case Step::Adopt:
        // Reported only once the adopt and the replay ran (SwitchSequence.h): a failed adopt or replay is the FAILED line.
        std::snprintf(out, sizeof(out), "adopt ok colorspace=%u hdr=%s", f.colorspace, f.hdr ? "on" : "off");
        break;
    case Step::Commit:
        std::snprintf(out, sizeof(out), "commit gen=%u hitchMs=%lld budgetLeft=%u", f.generation,
                      static_cast<long long>(f.ms), f.budgetLeft);
        break;
    case Step::Probation:
        if (f.fail != Fail::None)
            std::snprintf(out, sizeof(out), "probation failed reason=%s hr=0x%08X", FailName(f.fail), f.hr);
        else
            std::snprintf(out, sizeof(out), "probation ok presents=%u ms=%lld output=%s", f.presents,
                          static_cast<long long>(f.ms), f.fgActive ? "fg" : "plain");
        break;
    case Step::Rollback:
        // The target remembered as failed; the plain target prints "plain" here, never "none", which would read as
        // "nothing was remembered".
        std::snprintf(out, sizeof(out), "FAILED step=%s hr=0x%08X rollback=plain remember=%s", FailStepName(f.fail),
                      f.hr, f.to == Target::Plain ? "plain" : TargetText(f.to));
        break;
    case Step::Limp:
        std::snprintf(out, sizeof(out), "LIMP reason=%s rebuild=%s", FailName(f.fail), f.rebuild ? "yes" : "no");
        break;
    case Step::Dead:
        std::snprintf(out, sizeof(out), "DEAD reason=%s restart needed", FailName(f.fail));
        break;
    case Step::Detached:
        std::snprintf(out, sizeof(out), "detached plain chain rebuilt");
        break;
    default:
        std::snprintf(out, sizeof(out), "step=%s", StepName(f.step));
        break;
    }
}
} // namespace ext::fg::outswitch
