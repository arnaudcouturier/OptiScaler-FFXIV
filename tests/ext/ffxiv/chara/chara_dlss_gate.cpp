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

void TestDeviceIdentity()
{
    // Not compared until both sides are known.
    CHECK(CompareDevices({}, {}) == DeviceVerdict::NotCompared);
    CHECK(CompareDevices({ 0x1000, 0x1000 }, {}) == DeviceVerdict::NotCompared);
    CHECK(CompareDevices({}, { 0x1000, 0x1000 }) == DeviceVerdict::NotCompared);

    // One native device, plain pointers.
    CHECK(CompareDevices({ 0x1000, 0x1000 }, { 0x1000, 0x1000 }) == DeviceVerdict::Same);
    CHECK(CompareDevices({ 0x1000, 0x1000 }, { 0x2000, 0x2000 }) == DeviceVerdict::Different);

    // The regression of the first in-game test: the game's device is a wrapper (ReShade proxy). The pointer NGX was initialised
    // with, the one the context reports and the native device differ, but all of them name one device.
    const DeviceIdentity ngxProxy { 0x5000, 0x1000 };   // NGX got the proxy; a resource created through it says 0x1000
    const DeviceIdentity ctxNative { 0x1000, 0x1000 };  // the context returns the native device
    CHECK(CompareDevices(ngxProxy, ctxNative) == DeviceVerdict::Same);
    CHECK(CompareDevices(ctxNative, ngxProxy) == DeviceVerdict::Same);
    CHECK(CompareDevices({ 0x5000, 0x1000 }, { 0x6000, 0x1000 }) == DeviceVerdict::Same); // two different proxies of one device
    CHECK(ngxProxy.IsProxy() && !ctxNative.IsProxy());

    // Two proxies of two devices stay different, and so does a proxy against an unrelated native device.
    CHECK(CompareDevices({ 0x5000, 0x1000 }, { 0x6000, 0x2000 }) == DeviceVerdict::Different);
    CHECK(CompareDevices({ 0x5000, 0x1000 }, { 0x2000, 0x2000 }) == DeviceVerdict::Different);

    // A side whose native device could not be read falls back to its raw pointer: equal pointers match, others do not
    // (the path then stays off, which is the safe answer).
    CHECK(CompareDevices({ 0x1000, 0 }, { 0x1000, 0x1000 }) == DeviceVerdict::Same);
    CHECK(CompareDevices({ 0x5000, 0 }, { 0x1000, 0x1000 }) == DeviceVerdict::Different);
    const DeviceIdentity unreadable { 0x1000, 0 };
    CHECK(!unreadable.IsProxy());
}

void TestDeviceCache()
{
    DeviceCache cache;
    CHECK(cache.NeedsCheck(1, 0x10, 100)); // nothing cached yet

    cache.Store(DeviceVerdict::Same, 1, 0x10, 100);
    CHECK(!cache.NeedsCheck(1, 0x10, 100));
    CHECK(!cache.NeedsCheck(1, 0x10, 1000000)); // a good verdict does not expire on its own
    CHECK(cache.NeedsCheck(2, 0x10, 100));      // NGX initialised again (possibly on another device)
    CHECK(cache.NeedsCheck(1, 0x20, 100));      // another draw context

    // A "different" verdict is looked at again after a while, so it cannot stay stuck.
    cache.Store(DeviceVerdict::Different, 1, 0x10, 100);
    CHECK(!cache.NeedsCheck(1, 0x10, 100 + kDeviceRecheckMs - 1));
    CHECK(cache.NeedsCheck(1, 0x10, 100 + kDeviceRecheckMs));

    // mismatch, then NGX re-initialises on the device that draws: the gate clears
    GateFacts f = Ready();
    f.deviceChecked = true;
    f.deviceMatches = false;
    CHECK(Evaluate(f) == Availability::DeviceMismatch);
    f.deviceMatches = CompareDevices({ 0x5000, 0x1000 }, { 0x1000, 0x1000 }) == DeviceVerdict::Same;
    CHECK(Evaluate(f) == Availability::Available);

    // NGX not initialised, then initialised: the gate clears
    f = Ready();
    f.dx11Initialised = false;
    CHECK(Evaluate(f) == Availability::NgxNotInitialised);
    f.dx11Initialised = true;
    CHECK(Evaluate(f) == Availability::Available);

    cache.Reset();
    CHECK(cache.NeedsCheck(1, 0x10, 100));
}
} // namespace

int main()
{
    TestDecision();
    TestTexts();
    TestReasonLog();
    TestDeviceIdentity();
    TestDeviceCache();
    return TEST_RESULT("chara_dlss_gate");
}
