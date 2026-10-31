// The `[fg-switch]` line of every step (ext/fg/outswitch/SwitchLog.h): the exact text the game log and the benchmark
// driver read, one fixture per line of the verification plan. A field that moves, a token that becomes a sentence or a
// step that loses its numbers fails here.
#include "ext/fg/outswitch/SwitchLog.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>

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
std::string Line(const SwitchLogFields& fields)
{
    char line[512] = {};
    FormatSwitchLine(fields, line);
    return line;
}

// A provider chain's teardown row, as the product fills it.
SwitchLogFields TeardownRow()
{
    SwitchLogFields f;
    f.step = Step::Teardown;
    f.from = Target::XeFg;
    f.to = Target::Plain;
    f.cls = Class::XeFg;
    f.model = Model::M2;
    f.chain = 0x1C4A2B3D4E0ull;
    std::snprintf(f.libraryFile, sizeof(f.libraryFile), "%s", "libxess_fg.dll");
    f.librarySize = 12345678;
    f.libraryStamp = 0x65F1A2B3;
    f.pinReleased = true;
    f.witnessAlive = true;
    f.ms = 812;
    return f;
}

void EveryLineHasItsFields()
{
    SwitchLogFields begin;
    begin.step = Step::Begin;
    begin.generation = 4;
    begin.from = Target::FsrFg;
    begin.to = Target::XeFg;
    begin.why = "script";
    CHECK(Line(begin) == "begin gen=4 from=fsrfg to=xefg why=script");

    SwitchLogFields preflight;
    preflight.step = Step::Preflight;
    preflight.loadersFsr = true;
    preflight.loadersXe = true;
    preflight.thread = 14404;
    preflight.resizeGen = 0;
    CHECK(Line(preflight) == "preflight ok loaders=fsrfg,xefg thread=14404 resizeGen=0");
    preflight.loadersFsr = false;
    CHECK(Line(preflight) == "preflight ok loaders=xefg thread=14404 resizeGen=0");
    preflight.loadersXe = false;
    CHECK(Line(preflight) == "preflight ok loaders=none thread=14404 resizeGen=0");

    SwitchLogFields refused;
    refused.step = Step::Refused;
    refused.fail = Fail::Fullscreen;
    refused.severity = Severity::Transient;
    refused.to = Target::XeFg;
    CHECK(Line(refused) == "refused reason=Fullscreen class=transient target=xefg");

    SwitchLogFields barrier;
    barrier.step = Step::Barrier;
    barrier.ok = true;
    barrier.ms = 2;
    CHECK(Line(barrier) == "barrier ok waitedMs=2");
    barrier.ok = false;
    barrier.ms = 250;
    CHECK(Line(barrier) == "barrier failed waitedMs=250");

    SwitchLogFields drain;
    drain.step = Step::Drain;
    drain.gameDrained = true;
    drain.fgDrained = false;
    drain.ms = 117;
    CHECK(Line(drain) == "drain game=Drained fg=busy ms=117");

    CHECK(Line(TeardownRow()) ==
          "teardown old chain=000001C4A2B3D4E0 class=xefg lib=libxess_fg.dll/12345678/65F1A2B3 model=M2 "
          "pinRelease=yes witness=alive ms=812");
    SwitchLogFields m1 = TeardownRow();
    m1.model = Model::M1;
    m1.pinReleased = false;
    m1.witnessAlive = false;
    CHECK(Line(m1) ==
          "teardown old chain=000001C4A2B3D4E0 class=xefg lib=libxess_fg.dll/12345678/65F1A2B3 model=M1 "
          "pinRelease=no witness=na ms=812");
    SwitchLogFields noLib = TeardownRow();
    noLib.cls = Class::Plain;
    noLib.libraryFile[0] = '\0';
    noLib.model = Model::Unknown;
    noLib.witnessAlive = false;
    noLib.witnessPlaced = false; // a plain chain gets no witness: the line must not claim one died
    CHECK(Line(noLib) ==
          "teardown old chain=000001C4A2B3D4E0 class=plain lib=none model=unknown pinRelease=yes witness=na ms=812");
    SwitchLogFields unplaced = TeardownRow();
    unplaced.witnessPlaced = false; // a row without a witness on a provider class
    CHECK(Line(unplaced) ==
          "teardown old chain=000001C4A2B3D4E0 class=xefg lib=libxess_fg.dll/12345678/65F1A2B3 model=M2 "
          "pinRelease=yes witness=na ms=812");

    SwitchLogFields hooks;
    hooks.step = Step::Unhook;
    hooks.unhooked = 7;
    hooks.raw = 3;
    CHECK(Line(hooks) == "hooks unhooked=7 raw=3");

    SwitchLogFields create;
    create.step = Step::Create;
    create.ok = true;
    create.chain = 0x1C4A2B3D4F8ull;
    create.hr = 0;
    create.retries = 0;
    create.ms = 1543;
    CHECK(Line(create) == "create ok chain=000001C4A2B3D4F8 hr=0x00000000 retries=0 ms=1543");
    create.ok = false;
    create.hr = kAccessDenied;
    create.retries = 3;
    CHECK(Line(create) == "create failed chain=000001C4A2B3D4F8 hr=0x80070005 retries=3 ms=1543");

    SwitchLogFields rehook;
    rehook.step = Step::Rehook;
    rehook.vtbl = 0x7FFB2A3C1120ull;
    CHECK(Line(rehook) == "hooks class vtbl[8]=00007FFB2A3C1120");

    SwitchLogFields adopt;
    adopt.step = Step::Adopt;
    adopt.ok = true;
    adopt.colorspace = 0;
    adopt.hdr = false;
    CHECK(Line(adopt) == "adopt ok colorspace=0 hdr=off");
    adopt.hdr = true;
    adopt.colorspace = 12;
    CHECK(Line(adopt) == "adopt ok colorspace=12 hdr=on");
    // The adopt line is reported only after the adopt and the replay ran: there is no failed form of it.
    adopt.ok = false;
    CHECK(Line(adopt) == "adopt ok colorspace=12 hdr=on");

    SwitchLogFields commit;
    commit.step = Step::Commit;
    commit.generation = 4;
    commit.ms = 812;
    commit.budgetLeft = 5;
    CHECK(Line(commit) == "commit gen=4 hitchMs=812 budgetLeft=5");

    SwitchLogFields probation;
    probation.step = Step::Probation;
    probation.presents = 20;
    probation.ms = 8300;
    // output= says which output the judged chain is (a provider or plain), not whether frame generation ran.
    probation.fgActive = true;
    CHECK(Line(probation) == "probation ok presents=20 ms=8300 output=fg");
    probation.fgActive = false;
    CHECK(Line(probation) == "probation ok presents=20 ms=8300 output=plain");
    probation.fail = Fail::Stall;
    probation.hr = 0;
    CHECK(Line(probation) == "probation failed reason=Stall hr=0x00000000");

    SwitchLogFields rollback;
    rollback.step = Step::Rollback;
    rollback.fail = Fail::Create;
    rollback.hr = kAccessDenied;
    rollback.from = Target::FsrFg; // what was on the window
    rollback.to = Target::XeFg;    // the target that is remembered
    CHECK(Line(rollback) == "FAILED step=create hr=0x80070005 rollback=plain remember=xefg");
    rollback.fail = Fail::TestPresent;
    rollback.hr = kDeviceRemoved;
    CHECK(Line(rollback) == "FAILED step=test-present hr=0x887A0005 rollback=plain remember=xefg");
    // A plain target that failed is remembered as "plain", never "none" (which would read as nothing remembered).
    rollback.to = Target::Plain;
    rollback.fail = Fail::Create;
    rollback.hr = 0x80004005u;
    CHECK(Line(rollback) == "FAILED step=create hr=0x80004005 rollback=plain remember=plain");

    SwitchLogFields limp;
    limp.step = Step::Limp;
    limp.fail = Fail::PlainCreate;
    limp.rebuild = true;
    CHECK(Line(limp) == "LIMP reason=PlainCreate rebuild=yes");

    SwitchLogFields dead;
    dead.step = Step::Dead;
    dead.fail = Fail::OldChainLeaked;
    CHECK(Line(dead) == "DEAD reason=OldChainLeaked restart needed");
}

void TokensAreClosed()
{
    // Every reason prints a token, never the player-facing sentence: a tool can split the line on spaces.
    for (Fail fail : { Fail::NoBridge, Fail::NoModel, Fail::NoCapture, Fail::Budget, Fail::TeardownRefused,
                       Fail::ModelViolation, Fail::OldChainLeaked, Fail::Create, Fail::CreateDenied, Fail::Rehook,
                       Fail::Adopt, Fail::Replay, Fail::TestPresent, Fail::Probation, Fail::DeviceLost, Fail::Stall })
    {
        const char* name = FailName(fail);
        CHECK(name != nullptr && name[0] != '\0' && std::strcmp(name, "none") != 0);
        CHECK(std::strchr(name, ' ') == nullptr);
    }
    CHECK(std::strcmp(FailName(Fail::None), "none") == 0);
    CHECK(std::strcmp(FailStepName(Fail::Create), "create") == 0);
    CHECK(std::strcmp(FailStepName(Fail::TeardownRefused), "teardown") == 0);
    CHECK(std::strcmp(FailStepName(Fail::HookBusy), "hooks") == 0);
    CHECK(std::strcmp(FailStepName(Fail::TestPresent), "test-present") == 0);
    CHECK(std::strcmp(FailStepName(Fail::Stall), "probation") == 0);
}
} // namespace

int main()
{
    EveryLineHasItsFields();
    TokensAreClosed();
    if (g_failed != 0)
    {
        std::printf("fg_switch_log: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("fg_switch_log: all passed\n");
    return 0;
}
