#include "check.h"
#include "ext/ffxiv/chara/CharaDlssGate.h"

#include <initializer_list>

using namespace ext::ffxiv::chara::dlss;

namespace
{
GateFacts Ready()
{
    GateFacts f;
    f.configOn = true;
    f.ngxModuleLoaded = true;
    f.dlssEnabledInConfig = true;
    f.dx11Initialised = true;
    f.entryPoints = true;
    f.deviceChecked = true;
    f.deviceMatches = true;
    return f;
}

void TestDecision()
{
    CHECK(Evaluate(Ready()) == Availability::Available);

    GateFacts f = Ready();
    f.configOn = false;
    CHECK(Evaluate(f) == Availability::ConfigOff);

    // the config key wins over every other reason: a disabled feature stays silent
    f.latched = true;
    f.dx11Initialised = false;
    CHECK(Evaluate(f) == Availability::ConfigOff);

    f = Ready();
    f.latched = true;
    CHECK(Evaluate(f) == Availability::Latched);

    f = Ready();
    f.ngxModuleLoaded = false;
    CHECK(Evaluate(f) == Availability::NoNgxModule);

    f = Ready();
    f.dlssEnabledInConfig = false;
    CHECK(Evaluate(f) == Availability::DlssDisabledInConfig);

    f = Ready();
    f.dx11Initialised = false;
    CHECK(Evaluate(f) == Availability::NgxNotInitialised);

    f = Ready();
    f.entryPoints = false;
    CHECK(Evaluate(f) == Availability::EntryPointsMissing);

    f = Ready();
    f.deviceMatches = false;
    CHECK(Evaluate(f) == Availability::DeviceMismatch);

    // before the first draw the device was not compared yet: not a mismatch
    f = Ready();
    f.deviceChecked = false;
    f.deviceMatches = false;
    CHECK(Evaluate(f) == Availability::Available);

    // precedence: a missing module is reported before an uninitialised proxy
    f = Ready();
    f.ngxModuleLoaded = false;
    f.dx11Initialised = false;
    CHECK(Evaluate(f) == Availability::NoNgxModule);
}

void TestTexts()
{
    CHECK(ReasonText(Availability::Available)[0] == '\0');
    CHECK(ReasonText(Availability::ConfigOff)[0] == '\0');
    for (auto a : { Availability::Latched, Availability::NoNgxModule, Availability::DlssDisabledInConfig,
                    Availability::NgxNotInitialised, Availability::EntryPointsMissing, Availability::DeviceMismatch })
    {
        CHECK(ReasonText(a)[0] != '\0');
        CHECK(WorthLogging(a));
    }
    CHECK(!WorthLogging(Availability::Available));
    CHECK(!WorthLogging(Availability::ConfigOff));
}

void TestReasonLog()
{
    ReasonLog log;
    CHECK(!log.ShouldLog(Availability::ConfigOff)); // unchanged
    CHECK(log.ShouldLog(Availability::NgxNotInitialised));
    CHECK(!log.ShouldLog(Availability::NgxNotInitialised)); // one line per reason, however often it is evaluated
    CHECK(!log.ShouldLog(Availability::Available));         // good news is not logged
    CHECK(log.ShouldLog(Availability::NgxNotInitialised));   // it went away and came back: a new line

    // a flapping state cannot flood the log
    ReasonLog flap;
    uint32_t lines = 0;
    for (int i = 0; i < 100; ++i)
    {
        lines += flap.ShouldLog(Availability::NgxNotInitialised) ? 1 : 0;
        lines += flap.ShouldLog(Availability::Available) ? 1 : 0;
    }
    CHECK(lines == kMaxReasonLines);
}
} // namespace

int main()
{
    TestDecision();
    TestTexts();
    TestReasonLog();
    return TEST_RESULT("chara_dlss_gate");
}
