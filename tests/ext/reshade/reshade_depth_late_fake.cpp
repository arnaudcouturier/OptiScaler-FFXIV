// Automatic ReShade scene depth: the late registration and the event subscription against a stand-in ReShade DLL (fake_reshade.cpp),
// with the PRODUCTION ReShadeRegister.cpp, ReShadeDepthLate.cpp, ReShadeDepthEvents.cpp and ReShadeDepthCore.cpp.
// Usage: reshade_depth_late_fake.exe <late|bound|refuse> <path to FakeReShade64.dll>
// One mode per process: the ReShade helpers cache the module they resolved (and each event's entry point) in function-local statics.
//
//  late   : no ReShade yet (a failed attempt, the one-second interval, the option off), then the DLL is loaded: the next attempt
//           registers once, subscribes exactly the six depth events, each once and begin last, and nothing again afterwards; the
//           registered callbacks are distinct, reach the binder, do not look at the enabled flag, stand down while shutting down
//  bound  : ReShade never appears: 60 attempts, then no more
//  refuse : ReShade refuses API 20: one attempt, no retry, no event
//  crash  : ReShade faults inside ReShadeRegisterAddon: the guard turns it into a failed attempt, no retry, no event
//  failure: one event cannot be subscribed (it faults): it is remembered and not tried again, begin is never subscribed
//  race   : ReShade absent, many threads attempt at the same moment: one attempt
#include "pch.h"

#include "ext/reshade/ReShadeAddon.h"
#include "ext/reshade/ReShadeDepthDispatch.h"
#include "ext/reshade/ReShadeDepthEvents.h"
#include "ext/reshade/ReShadeDepthLate.h"
#include "ext/reshade/ReShadeRegistrationState.h"
#include "ext/reshade/third_party/reshade/reshade.hpp"

#include "reshade_depth_fake.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace depthtest;
using namespace ext::reshade;

// ---- what ReShadeAddon.cpp provides in the product beyond the registration state (it needs the whole config to build): Status() ------
namespace ext::reshade
{
AddonStatus Status()
{
    AddonStatus s;
    s.attempted = Registration().attempted.load(); // set by main: the load-time attempt ran and found no ReShade
    s.registered = Registration().registered.load();
    s.reason = static_cast<RegisterStatus>(Registration().reason.load());
    return s;
}
} // namespace ext::reshade

namespace
{
using InfoFn = void (*)(uint32_t*, void**, int*, int*, int*, void**);
using EventsFn = int (*)(int*, void**, int);
using EventCallsFn = int (*)();
using ::reshade::addon_event;
using ::reshade::api::command_list;
using ::reshade::api::effect_runtime;
using ::reshade::api::effect_technique;
using ::reshade::api::resource_view;

using BeginFn = void (*)(effect_runtime*, command_list*, resource_view, resource_view);
using RuntimeFn = void (*)(effect_runtime*);
using TechniqueFn = bool (*)(effect_runtime*, effect_technique, bool);
using PresetFn = void (*)(effect_runtime*, const char*);

struct Registered
{
    int events[64] = {};
    void* callbacks[64] = {};
    int count = 0;
    void* Find(addon_event ev) const
    {
        for (int i = 0; i < count && i < 64; ++i)
            if (events[i] == static_cast<int>(ev))
                return callbacks[i];
        return nullptr;
    }
    int Times(addon_event ev) const
    {
        int n = 0;
        for (int i = 0; i < count && i < 64; ++i)
            n += events[i] == static_cast<int>(ev) ? 1 : 0;
        return n;
    }
};

size_t LogLines(const char* needle)
{
    size_t n = 0;
    for (const std::string& line : depthtest_log::Lines())
        n += line.find(needle) != std::string::npos ? 1 : 0;
    return n;
}

std::string LastLine(const char* needle)
{
    std::string found;
    for (const std::string& line : depthtest_log::Lines())
        if (line.find(needle) != std::string::npos)
            found = line;
    return found;
}

Registered ReadEvents(HMODULE fake)
{
    Registered r;
    auto fn = reinterpret_cast<EventsFn>(GetProcAddress(fake, "Fake_Events"));
    if (fn != nullptr)
        r.count = fn(r.events, r.callbacks, 64);
    return r;
}

struct FakeHost final : IEventHost
{
    void NoteRuntime(uint64_t) override {}
    void ForgetRuntime(uint64_t) override {}
};

effect_runtime* Rt() { return reinterpret_cast<effect_runtime*>(kRt); }

const addon_event kSix[] = { addon_event::reshade_begin_effects,   addon_event::reshade_finish_effects,
                             addon_event::destroy_effect_runtime,  addon_event::reshade_reloaded_effects,
                             addon_event::reshade_set_technique_state, addon_event::reshade_set_current_preset_path };

int Late(HMODULE& fake, const char* dll)
{
    Fake world;
    FakeHost host;
    std::atomic<uint64_t> counter { 100 };
    world.frameSource = &counter;
    DepthBinder binder(world, nullptr);
    EventDispatcher dispatcher(binder, host);
    SetDispatcher(&dispatcher);

    // ---- ReShade is not loaded
    CHECK(!TryLateRegister(true, 10'000));
    CHECK(GetLateStatus().attempts == 1 && !Status().registered && Status().reason == RegisterStatus::NoModule);
    CHECK(!TryLateRegister(true, 10'500)); // under a second after the last attempt
    CHECK(GetLateStatus().attempts == 1);
    CHECK(!TryLateRegister(false, 12'000)); // the option is off: no attempt at all
    CHECK(GetLateStatus().attempts == 1);
    CHECK(SubscribedEvents() == 0);

    // ---- ReShade loads later
    fake = LoadLibraryA(dll);
    CHECK(fake != nullptr);
    if (fake == nullptr)
        return 1;
    auto info = reinterpret_cast<InfoFn>(GetProcAddress(fake, "Fake_Info"));
    CHECK(info != nullptr);
    CHECK(ReadEvents(fake).count == 0); // loading it subscribes nothing

    CHECK(TryLateRegister(true, 20'000));
    CHECK(Status().registered && Status().reason == RegisterStatus::Ok);
    uint32_t api = 0;
    void* module = nullptr;
    int calls = 0, events = 0, lastEvent = 0;
    void* lastCallback = nullptr;
    info(&api, &module, &calls, &events, &lastEvent, &lastCallback);
    CHECK(calls == 1 && api == 20);
    CHECK(module == GetModuleHandleW(nullptr)); // registered as this module (the test exe stands in for the add-on)

    // exactly the six depth events, each once, begin last
    Registered r = ReadEvents(fake);
    CHECK(r.count == 6);
    for (const addon_event ev : kSix)
        CHECK(r.Times(ev) == 1);
    std::set<void*> distinct(r.callbacks, r.callbacks + std::min(r.count, 64));
    CHECK(distinct.size() == 6 && distinct.count(nullptr) == 0); // six different callbacks
    CHECK(r.count == 6 && r.events[5] == static_cast<int>(addon_event::reshade_begin_effects));
    CHECK(SubscribedEvents() == kAllDepthEvents && FailedEvents() == 0);
    // the subscription says which thread made it (whether that is ReShade's own dispatch thread decides if it can race its event lists)
    CHECK(LogLines("subscribed to") == 1);
    CHECK(LastLine("subscribed to").find("on thread " + std::to_string(GetCurrentThreadId())) != std::string::npos);
    CHECK(LastLine("subscribed to").find("late (") != std::string::npos && LastLine("subscribed to").find("at load") == std::string::npos);
    // not one of the other events the add-on subscribes elsewhere (boundary tracking, probe, window mode, present)
    for (int i = 0; i < r.count; ++i)
    {
        const int ev = r.events[i];
        CHECK(ev != static_cast<int>(addon_event::init_effect_runtime) && ev != static_cast<int>(addon_event::reshade_present) &&
              ev != static_cast<int>(addon_event::present) && ev != static_cast<int>(addon_event::set_fullscreen_state));
    }

    // never twice: another attempt, another subscribe call
    CHECK(!TryLateRegister(true, 30'000));
    CHECK(EnsureSubscribed());
    CHECK(EnsureSubscribed());
    info(&api, &module, &calls, &events, &lastEvent, &lastCallback);
    CHECK(calls == 1 && ReadEvents(fake).count == 6 && GetLateStatus().attempts == 2);

    // ---- the callbacks that were registered
    auto begin = reinterpret_cast<BeginFn>(r.Find(addon_event::reshade_begin_effects));
    auto finish = reinterpret_cast<BeginFn>(r.Find(addon_event::reshade_finish_effects));
    auto destroy = reinterpret_cast<RuntimeFn>(r.Find(addon_event::destroy_effect_runtime));
    auto reloaded = reinterpret_cast<RuntimeFn>(r.Find(addon_event::reshade_reloaded_effects));
    auto technique = reinterpret_cast<TechniqueFn>(r.Find(addon_event::reshade_set_technique_state));
    auto preset = reinterpret_cast<PresetFn>(r.Find(addon_event::reshade_set_current_preset_path));
    CHECK(begin && finish && destroy && reloaded && technique && preset);
    if (!(begin && finish && destroy && reloaded && technique && preset))
        return 1;

    binder.SetEnabled(true);
    auto publish = [&]
    {
        DepthCapture c;
        c.valid = true;
        c.outputW = 3840;
        c.outputH = 2160;
        c.renderW = 1920;
        c.renderH = 1080;
        c.frame = ++counter; // one tick per capture
        c.tickMs = world.now;
        c.copyView = kCopy;
        c.device = kDevice;
        binder.PublishCapture(c);
    };
    // the first begin and the first finish callback say which thread ReShade runs them on, once: here a worker thread
    {
        DWORD workerId = 0;
        std::thread worker(
            [&]
            {
                workerId = GetCurrentThreadId();
                begin(Rt(), nullptr, resource_view {}, resource_view {});
                finish(Rt(), nullptr, resource_view {}, resource_view {});
                begin(Rt(), nullptr, resource_view {}, resource_view {});
                finish(Rt(), nullptr, resource_view {}, resource_view {});
            });
        worker.join();
        CHECK(LogLines("first begin_effects callback") == 1 && LogLines("first finish_effects callback") == 1);
        CHECK(LastLine("first begin_effects callback").find("on thread " + std::to_string(workerId)) != std::string::npos);
        CHECK(LastLine("first finish_effects callback").find("on thread " + std::to_string(workerId)) != std::string::npos);
    }
    const uint64_t base = counter.load(); // (the passes of the worker ticked nothing: no event moves the counter)
    CHECK(base == 100);
    publish();
    begin(Rt(), nullptr, resource_view {}, resource_view {});
    CHECK(LogLines("first begin_effects callback") == 1); // not again, from this thread
    CHECK(world.DepthIs(kCopy, kCopy)); // the binder was reached through the registered pointer
    // the option is switched off while the pass is open: the registered finish callback still puts the binding back
    binder.SetEnabled(false);
    finish(Rt(), nullptr, resource_view {}, resource_view {});
    CHECK(world.DepthIs(kOrig, kOrigSrgb) && !world.HoldsCopy());
    CHECK(counter == base + 1); // (the one publish above)

    // the technique-state callback never vetoes and clears the cache; the others are plain
    binder.SetEnabled(true);
    world.now += 16;
    publish();
    begin(Rt(), nullptr, resource_view {}, resource_view {});
    finish(Rt(), nullptr, resource_view {}, resource_view {});
    const int queries = world.defQueries;
    CHECK(!technique(Rt(), effect_technique { 1 }, true));
    preset(Rt(), "preset.ini");
    world.now += 16;
    publish();
    begin(Rt(), nullptr, resource_view {}, resource_view {});
    finish(Rt(), nullptr, resource_view {}, resource_view {});
    CHECK(world.defQueries > queries);
    reloaded(Rt());
    destroy(Rt());

    // shutting down: the callbacks stand down
    State::Instance().isShuttingDown = true;
    const int before = world.calls;
    world.now += 16;
    publish();
    begin(Rt(), nullptr, resource_view {}, resource_view {});
    finish(Rt(), nullptr, resource_view {}, resource_view {});
    destroy(Rt());
    CHECK(technique(Rt(), effect_technique { 1 }, true) == false);
    CHECK(world.calls == before);
    State::Instance().isShuttingDown = false;

    // no dispatcher yet: nothing happens either
    SetDispatcher(nullptr);
    begin(Rt(), nullptr, resource_view {}, resource_view {});
    CHECK(world.calls == before);
    SetDispatcher(&dispatcher);
    return 0;
}

int Race()
{
    constexpr int kThreads = 16;
    std::atomic<bool> go { false };
    std::atomic<int> ready { 0 };
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i)
        threads.emplace_back(
            [&]
            {
                ++ready;
                while (!go.load())
                    std::this_thread::yield();
                TryLateRegister(true, 10'000);
            });
    while (ready.load() < kThreads)
        std::this_thread::yield();
    go = true;
    for (std::thread& t : threads)
        t.join();
    CHECK(GetLateStatus().attempts == 1); // single flight: one thread made the attempt, the others saw it
    CHECK(!TryLateRegister(true, 10'500));
    CHECK(GetLateStatus().attempts == 1);
    return 0;
}

int Crash(HMODULE& fake, const char* dll)
{
    CHECK(!TryLateRegister(true, 10'000)); // not loaded yet
    fake = LoadLibraryA(dll);
    CHECK(fake != nullptr);
    if (fake == nullptr)
        return 1;
    CHECK(!TryLateRegister(true, 20'000)); // the fake faults inside ReShadeRegisterAddon: caught, a failed attempt
    CHECK(!Status().registered && Status().reason == RegisterStatus::Exception);
    CHECK(!TryLateRegister(true, 40'000)); // and final
    CHECK(GetLateStatus().attempts == 2);
    CHECK(ReadEvents(fake).count == 0 && SubscribedEvents() == 0);
    return 0;
}

// The normal case: ReShade is there when OptiScaler loads. OnReshadeLoaded registers and subscribes the six events right away, before
// anything can dispatch and before the facade exists: no register_event from the evaluate thread, no race with ReShade's dispatch.
int Load(HMODULE& fake, const char* dll)
{
    fake = LoadLibraryA(dll);
    CHECK(fake != nullptr);
    if (fake == nullptr)
        return 1;
    auto info = reinterpret_cast<InfoFn>(GetProcAddress(fake, "Fake_Info"));
    CHECK(info != nullptr && ReadEvents(fake).count == 0);

    // OnReshadeLoaded: register, note it, subscribe. (The test exe stands in for the add-on module.)
    const RegisterResult result = RegisterAddon(GetModuleHandleW(nullptr), nullptr);
    CHECK(result.status == RegisterStatus::Ok);
    NoteRegistration(result.status);
    CHECK(SubscribedEvents() == 0 && ReadEvents(fake).count == 0); // registering subscribes nothing by itself
    SetDispatcher(nullptr);                                          // nothing built yet: no facade, no dispatcher
    CHECK(EnsureSubscribed(SubscribeWhen::Load));

    // exactly the six depth events, each once, begin last
    Registered r = ReadEvents(fake);
    CHECK(r.count == 6);
    for (const addon_event ev : kSix)
        CHECK(r.Times(ev) == 1);
    CHECK(r.count == 6 && r.events[5] == static_cast<int>(addon_event::reshade_begin_effects));
    CHECK(SubscribedEvents() == kAllDepthEvents && FailedEvents() == 0);
    CHECK(LogLines("subscribed to") == 1);
    CHECK(LastLine("subscribed to").find("at load") != std::string::npos && LastLine("subscribed to").find("late (") == std::string::npos);
    CHECK(LastLine("subscribed to").find("on thread " + std::to_string(GetCurrentThreadId())) != std::string::npos);

    // later: the first enable (EnsureSubscribed from the evaluate thread), the late planner and a second load call add nothing
    CHECK(EnsureSubscribed());
    CHECK(EnsureSubscribed(SubscribeWhen::Load));
    CHECK(!TryLateRegister(true, 20'000)); // registered: the planner never registers twice
    CHECK(GetLateStatus().attempts == 0);
    int calls = 0, events = 0, lastEvent = 0;
    uint32_t api = 0;
    void* module = nullptr;
    void* lastCallback = nullptr;
    info(&api, &module, &calls, &events, &lastEvent, &lastCallback);
    CHECK(calls == 1 && ReadEvents(fake).count == 6 && LogLines("subscribed to") == 1);

    // the callbacks tolerate a facade that does not exist: nothing is read, nothing is called
    auto begin = reinterpret_cast<BeginFn>(r.Find(addon_event::reshade_begin_effects));
    auto finish = reinterpret_cast<BeginFn>(r.Find(addon_event::reshade_finish_effects));
    auto destroy = reinterpret_cast<RuntimeFn>(r.Find(addon_event::destroy_effect_runtime));
    auto reloaded = reinterpret_cast<RuntimeFn>(r.Find(addon_event::reshade_reloaded_effects));
    auto technique = reinterpret_cast<TechniqueFn>(r.Find(addon_event::reshade_set_technique_state));
    auto preset = reinterpret_cast<PresetFn>(r.Find(addon_event::reshade_set_current_preset_path));
    CHECK(begin && finish && destroy && reloaded && technique && preset);
    if (!(begin && finish && destroy && reloaded && technique && preset))
        return 1;
    Fake world;
    FakeHost host;
    std::atomic<uint64_t> counter { 100 };
    world.frameSource = &counter;
    DepthBinder binder(world, nullptr);
    EventDispatcher dispatcher(binder, host);
    binder.SetEnabled(true);
    for (int i = 0; i < 3; ++i)
    {
        begin(Rt(), nullptr, resource_view {}, resource_view {});
        finish(Rt(), nullptr, resource_view {}, resource_view {});
        destroy(Rt());
        reloaded(Rt());
        CHECK(!technique(Rt(), effect_technique { 1 }, true));
        preset(Rt(), "preset.ini");
    }
    CHECK(world.log.empty() && counter == 100); // not one backend call, no frame tick
    CHECK(LogLines("first begin_effects callback") == 0 && LogLines("first finish_effects callback") == 0);

    // the facade is built at the first enable and installs its dispatcher: the same registered callbacks now reach the binder
    SetDispatcher(&dispatcher);
    DepthCapture c;
    c.valid = true;
    c.outputW = 3840;
    c.outputH = 2160;
    c.renderW = 1920;
    c.renderH = 1080;
    c.frame = ++counter; // one tick per capture
    c.tickMs = world.now;
    c.copyView = kCopy;
    c.device = kDevice;
    binder.PublishCapture(c);
    begin(Rt(), nullptr, resource_view {}, resource_view {});
    CHECK(world.DepthIs(kCopy, kCopy));
    finish(Rt(), nullptr, resource_view {}, resource_view {});
    CHECK(world.DepthIs(kOrig, kOrigSrgb) && !world.HoldsCopy() && counter == 101);
    CHECK(LogLines("first begin_effects callback") == 1);
    return 0;
}

int Failure(HMODULE& fake, const char* dll)
{
    Fake world;
    FakeHost host;
    std::atomic<uint64_t> counter { 100 };
    DepthBinder binder(world, nullptr);
    EventDispatcher dispatcher(binder, host);
    SetDispatcher(&dispatcher);
    fake = LoadLibraryA(dll);
    CHECK(fake != nullptr);
    if (fake == nullptr)
        return 1;
    auto calls = reinterpret_cast<EventCallsFn>(GetProcAddress(fake, "Fake_EventCalls"));
    CHECK(TryLateRegister(true, 20'000)); // ReShade accepts; the finish event faults while it is subscribed
    CHECK(calls() == 5);                  // finish (faulted), destroy, reloaded, technique state, preset path; begin was never tried
    const Registered r = ReadEvents(fake);
    CHECK(r.count == 4 && r.Times(addon_event::reshade_begin_effects) == 0 && r.Times(addon_event::reshade_finish_effects) == 0);
    const uint32_t finishBit = 1u << static_cast<uint32_t>(DepthEvent::FinishEffects);
    const uint32_t beginBit = 1u << static_cast<uint32_t>(DepthEvent::BeginEffects);
    CHECK(SubscribedEvents() == (kAllDepthEvents & ~finishBit & ~beginBit));
    CHECK(FailedEvents() == (finishBit | beginBit));
    CHECK(!EnsureSubscribed());
    CHECK(!EnsureSubscribed());
    CHECK(calls() == 5 && ReadEvents(fake).count == 4); // remembered: not tried again
    return 0;
}

int Bound()
{
    for (int i = 0; i < 100; ++i)
        TryLateRegister(true, 10'000 + static_cast<int64_t>(i) * 1100);
    CHECK(GetLateStatus().attempts == LateRegistrationPlanner::kMaxAttempts);
    CHECK(GetLateStatus().gaveUp && !Status().registered && Status().reason == RegisterStatus::NoModule);
    const int64_t last = GetLateStatus().lastAttemptMs;
    CHECK(!TryLateRegister(true, last + 100'000));
    CHECK(GetLateStatus().attempts == LateRegistrationPlanner::kMaxAttempts);
    return 0;
}

int Refuse(HMODULE& fake, const char* dll)
{
    CHECK(!TryLateRegister(true, 10'000)); // not loaded yet
    fake = LoadLibraryA(dll);
    CHECK(fake != nullptr);
    if (fake == nullptr)
        return 1;
    CHECK(!TryLateRegister(true, 20'000));
    CHECK(!Status().registered && Status().reason == RegisterStatus::Refused);
    auto info = reinterpret_cast<InfoFn>(GetProcAddress(fake, "Fake_Info"));
    uint32_t api = 0;
    void* module = nullptr;
    int calls = 0, events = 0, lastEvent = 0;
    void* lastCallback = nullptr;
    info(&api, &module, &calls, &events, &lastEvent, &lastCallback);
    CHECK(calls == 1);
    CHECK(!TryLateRegister(true, 40'000)); // a refusal is final: no second attempt
    info(&api, &module, &calls, &events, &lastEvent, &lastCallback);
    CHECK(calls == 1 && ReadEvents(fake).count == 0 && SubscribedEvents() == 0);
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
        return 2;
    const std::string mode = argv[1];
    ext::reshade::Registration().attempted = true; // the load-time attempt ran (and found no ReShade, except in the mode that IS the load)
    SetEnvironmentVariableA("FAKE_RESHADE_MODE", mode == "refuse" ? "refuse" : mode == "crash" ? "crash" : "ok");
    if (mode == "failure")
        SetEnvironmentVariableA("FAKE_RESHADE_CRASH_EVENT", std::to_string(static_cast<int>(addon_event::reshade_finish_effects)).c_str());
    HMODULE fake = nullptr;
    int rc = 2;
    if (mode == "late")
        rc = Late(fake, argv[2]);
    else if (mode == "load")
        rc = Load(fake, argv[2]);
    else if (mode == "bound")
        rc = Bound();
    else if (mode == "refuse")
        rc = Refuse(fake, argv[2]);
    else if (mode == "crash")
        rc = Crash(fake, argv[2]);
    else if (mode == "failure")
        rc = Failure(fake, argv[2]);
    else if (mode == "race")
        rc = Race();
    if (rc != 0 || g_fail != 0)
    {
        std::printf("FAIL (%s): %d check(s) failed\n", mode.c_str(), g_fail);
        return 1;
    }
    std::printf("PASS (%s)\n", mode.c_str());
    return 0;
}
