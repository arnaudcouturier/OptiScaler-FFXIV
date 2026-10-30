// The product side of the probe (ext/latency/TickProbe.cpp) with a fake clock, a fake config key and a recording sink: the key-off path produces no
// event and no clock read, the probe arms only on top of an installed lease (and a tick that was in flight at arming is dropped, not half-recorded), a
// config reload applies the key live without a per-frame ini read, the note de-duplication holds, and the 10 s window writes its line through the default
// log sink.
//
// The fake clock is what makes the window testable: QueryPerformanceFrequency is 1,000,000, so the probe's QueryPerformanceCounter read is the
// microsecond value the test sets. The source pins at the end cover the one rule a run cannot observe from outside: with the key off, the per-present
// path is one relaxed load of the mirrored key.
#include "pch.h"

#include "../ffxiv/chara/check.h"

#include "ext/core/ExtConfig.h"
#include "ext/ffxiv/ActiveBuild.h"
#include "ext/latency/TickProbe.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
std::int64_t g_nowUs = 0;
ext::ExtConfigProbeStub& Config()
{
    static ext::ExtConfigProbeStub config;
    return config;
}
ext::KeysLoadedHook g_hooks[ext::kMaxKeysLoadedHooks] {};
std::size_t g_hookCount = 0;
bool g_leased = false;
bool g_counterAvailable = false;
std::uint32_t g_counterValue = 0;

ext::ffxiv::detail::ActiveState& Active()
{
    static ext::ffxiv::detail::ActiveState* state = []
    {
        auto* created = new ext::ffxiv::detail::ActiveState {};
        created->gate.started = true;
        created->gate.At(ext::ffxiv::resolve::Feature::LatencyTick).enabled = true;
        created->gate.At(ext::ffxiv::resolve::Feature::LatencyTick).reason = "the test published the feature";
        ext::ffxiv::detail::g_active.store(created, std::memory_order_release);
        return created;
    }();
    return *state;
}

struct Event
{
    enum class Kind
    {
        TickBegin,
        TickEnd,
        PresentBegin,
        PresentEnd,
        SleepBegin,
        SleepEnd,
        Note,
    };
    Kind kind;
    std::int64_t qpcUs = 0;
    std::uint32_t tid = 0;
    std::uint32_t frame = 0;
    bool counter = false;
    std::string text;
};

struct Recorder final : ext::latency::IProbeSink
{
    std::vector<Event> events;
    void OnTickBegin(std::int64_t qpcUs, std::uint32_t tid, std::uint32_t frameBefore, bool frameCounter) override
    {
        events.push_back({ Event::Kind::TickBegin, qpcUs, tid, frameBefore, frameCounter, {} });
    }
    void OnTickEnd(std::int64_t qpcUs, std::uint32_t frameAfter) override { events.push_back({ Event::Kind::TickEnd, qpcUs, 0, frameAfter, false, {} }); }
    void OnPresentBegin(std::int64_t qpcUs, std::uint32_t tid, std::uint32_t frameAtBegin, bool frameCounter) override
    {
        events.push_back({ Event::Kind::PresentBegin, qpcUs, tid, frameAtBegin, frameCounter, {} });
    }
    void OnPresentEnd(std::int64_t qpcUs, std::uint32_t frameAtEnd) override
    {
        events.push_back({ Event::Kind::PresentEnd, qpcUs, 0, frameAtEnd, false, {} });
    }
    void OnSleepBegin(std::int64_t qpcUs) override { events.push_back({ Event::Kind::SleepBegin, qpcUs, 0, 0, false, {} }); }
    void OnSleepEnd(std::int64_t qpcUs) override { events.push_back({ Event::Kind::SleepEnd, qpcUs, 0, 0, false, {} }); }
    void OnNote(const char* text) override { events.push_back({ Event::Kind::Note, 0, 0, 0, false, text != nullptr ? text : "" }); }
};

Recorder g_recorder;

void ResetRecording()
{
    g_recorder.events.clear();
    g_nowUs = 0;
}

void SetKey(bool on)
{
    Config().framePacing.on = on;
    Config().PostLoad(); // the product registered its mirror on this hook; a reload is how the key applies live
}

void SetLeased(bool leased) { g_leased = leased; }

// One present begin/end at the current fake time, carrying the counter value the test set. `frameCounter` false models a build without the offset.
void Present(bool frameCounter = true)
{
    g_counterAvailable = frameCounter;
    ext::latency::OnBridgePresentBegin();
    ext::latency::OnBridgePresentEnd();
}

void TestKeyOffCostsOneLoad()
{
    ResetRecording();
    SetLeased(true);
    SetKey(false);
    CHECK(!ext::latency::KeyOn());
    g_nowUs = 1000;
    Present();
    ext::latency::OnTickBegin(1, true);
    ext::latency::OnTickEnd(2);
    ext::latency::OnSleepScopeBegin();
    ext::latency::OnSleepScopeEnd();
    CHECK(g_recorder.events.empty()); // no event, no clock read, no note
    CHECK(!ext::latency::Armed());

    // a present with the key off after the probe was armed: disarmed, with one note, and nothing else
    SetKey(true);
    Present();
    CHECK(ext::latency::Armed());
    g_recorder.events.clear();
    SetKey(false);
    Present();
    CHECK(!ext::latency::Armed());
    CHECK(g_recorder.events.size() == 1 && g_recorder.events[0].kind == Event::Kind::Note);
    CHECK(g_recorder.events[0].text.find("paused") != std::string::npos);
}

void TestArmingNeedsTheLease()
{
    ResetRecording();
    SetKey(true);
    SetLeased(false);
    Present();
    CHECK(!ext::latency::Armed());
    CHECK(g_recorder.events.empty()); // the key is on, but nothing is recorded before the lease is in the slot

    // a tick while unarmed is not recorded either: the taps see the same arm state, so the tick that is in flight when the lease arrives never
    // reaches the sink (the observer does not even read the counter for it).
    ext::latency::OnTickBegin(10, true);
    ext::latency::OnTickEnd(11);
    CHECK(g_recorder.events.empty());

    // the next present arms the probe; the ticks after it are recorded normally
    SetLeased(true);
    Present();
    CHECK(ext::latency::Armed());
    CHECK(g_recorder.events.size() == 2); // the present begin and end of the arming present
    g_recorder.events.clear();
    g_nowUs += 500;
    ext::latency::OnTickBegin(11, true); // frame 12
    g_nowUs += 1000;
    ext::latency::OnTickEnd(12);
    CHECK(g_recorder.events.size() == 2);
    CHECK(g_recorder.events[0].kind == Event::Kind::TickBegin && g_recorder.events[0].frame == 11 && g_recorder.events[0].counter);
    CHECK(g_recorder.events[1].kind == Event::Kind::TickEnd && g_recorder.events[1].frame == 12);
}

void TestEventsCarryTheCounter()
{
    ResetRecording();
    SetKey(true);
    SetLeased(true);
    g_counterValue = 40;
    g_counterAvailable = true;
    g_nowUs = 1'000'000;
    ext::latency::OnBridgePresentBegin();
    CHECK(g_recorder.events.size() == 1);
    CHECK(g_recorder.events[0].kind == Event::Kind::PresentBegin && g_recorder.events[0].frame == 40 && g_recorder.events[0].counter);
    CHECK(g_recorder.events[0].qpcUs == 1'000'000);
    CHECK(g_recorder.events[0].tid == GetCurrentThreadId());
    g_nowUs = 1'001'000;
    ext::latency::OnBridgePresentEnd();
    CHECK(g_recorder.events.size() == 2 && g_recorder.events[1].kind == Event::Kind::PresentEnd && g_recorder.events[1].frame == 40);

    // a tick, with the counter the observer read on each side
    g_nowUs = 1'002'000;
    ext::latency::OnTickBegin(40, true);
    g_nowUs = 1'004'000;
    ext::latency::OnTickEnd(41);
    CHECK(g_recorder.events.size() == 4);
    CHECK(g_recorder.events[2].kind == Event::Kind::TickBegin && g_recorder.events[2].frame == 40 && g_recorder.events[2].counter);
    CHECK(g_recorder.events[3].kind == Event::Kind::TickEnd && g_recorder.events[3].frame == 41);

    // a build without the counter: the flag reaches the sink, which is what makes the window print n/a
    g_recorder.events.clear();
    g_nowUs = 1'005'000;
    ext::latency::OnTickBegin(0, false);
    g_nowUs = 1'007'000;
    ext::latency::OnTickEnd(0);
    CHECK(g_recorder.events.size() == 2 && !g_recorder.events[0].counter && g_recorder.events[0].frame == 0);

    // the sleep taps are only reached while armed
    g_recorder.events.clear();
    ext::latency::OnSleepScopeBegin();
    g_nowUs = 1'008'000;
    ext::latency::OnSleepScopeEnd();
    CHECK(g_recorder.events.size() == 2 && g_recorder.events[0].kind == Event::Kind::SleepBegin && g_recorder.events[1].kind == Event::Kind::SleepEnd);
}

void TestNoteIsDeduplicated()
{
    ResetRecording();
    SetKey(true);
    ext::latency::Note("installed x");
    ext::latency::Note("installed x"); // the same text twice in a row says nothing new
    ext::latency::Note("refused: y");
    ext::latency::Note("installed x");
    CHECK(g_recorder.events.size() == 3);
    CHECK(g_recorder.events[0].text == "installed x" && g_recorder.events[1].text == "refused: y" && g_recorder.events[2].text == "installed x");
}

void TestWindowLineThroughTheLogSink()
{
    // The log sink has never been the target, so its Probe is empty. Rebuild a small paired window by hand and close it with an event 10 s later: the
    // line goes to the captured log.
    ext::latency::SetEventSink(nullptr);
    ResetRecording();
    SetKey(true);
    SetLeased(true);
    g_nowUs = 0;
    Present(); // arms the log sink's probe and records present begin/end at 0
    ext::latency::OnTickBegin(7, true); // frame 8
    g_nowUs = 2000;
    ext::latency::OnTickEnd(8);
    g_nowUs = 7000;
    g_counterValue = 8;
    Present(); // this present carries frame 8 -> paired, age 7000
    const int lines = g_testLogLines;
    g_nowUs = 10'000'000;
    ext::latency::OnTickBegin(8, true); // closes the window, part of it
    g_nowUs = 10'002'000;
    ext::latency::OnTickEnd(9);
    CHECK(g_testLogLines > lines);
    const std::string& line = g_testLastLog;
    if (line.find("Tick probe:") == std::string::npos)
        std::printf("FAIL: no tick line was logged (last: %s)\n", line.c_str());
    CHECK(line.find("Tick probe:") != std::string::npos);
    CHECK(line.find("ticks=") != std::string::npos);
    CHECK(line.find("counter ok") != std::string::npos); // the fake counter advanced by one per tick
    CHECK(line.find("paired ") != std::string::npos);
    ext::latency::SetEventSink(&g_recorder);
}

// The taps nested the way FFXIV runs them: the bridge present begins and ends between the tick's begin and end, on this thread. The window's line
// must say `same`, report W as the present's own duration, and leave the queue wait at n/a. The arming present sits in a window of its own so the
// measured window holds exactly one tick and one present; the counter numbering continues from the window test before this one (9 -> 10 -> 11 ->
// 12), because the log sink keeps one probe for the process.
void TestSameThreadWindowLineThroughTheLogSink()
{
    ext::latency::SetEventSink(nullptr);
    ResetRecording();
    SetKey(true);
    SetLeased(true);
    g_nowUs = 0;
    Present(); // the arming present, outside any tick; it opens the first window
    g_nowUs = 10'000'000;
    ext::latency::OnTickBegin(9, true); // frame 10; closes the arming window
    g_nowUs = 10'001'000;
    ext::latency::OnTickEnd(10);

    // the measured window: one tick, with the present and the Reflex sleep inside it
    g_counterValue = 11;
    g_nowUs = 10'002'000;
    ext::latency::OnTickBegin(10, true); // frame 11, opens the window
    g_nowUs = 10'006'000;
    ext::latency::OnBridgePresentBegin();
    g_nowUs = 10'006'500;
    ext::latency::OnSleepScopeBegin();
    g_nowUs = 10'007'000;
    ext::latency::OnSleepScopeEnd();
    g_nowUs = 10'009'000;
    ext::latency::OnBridgePresentEnd();
    g_nowUs = 10'011'000;
    ext::latency::OnTickEnd(11);
    const int lines = g_testLogLines;
    g_nowUs = 20'004'000;
    ext::latency::OnTickBegin(11, true); // frame 12; closes the window, part of it
    g_nowUs = 20'006'000;
    ext::latency::OnTickEnd(12);
    CHECK(g_testLogLines > lines);
    const std::string& line = g_testLastLog;
    if (line.find("same") == std::string::npos)
        std::printf("FAIL: the same-thread tick line did not say same (last: %s)\n", line.c_str());
    CHECK(line.find("W p50=3.1 p95=3.1") != std::string::npos); // the present's duration, 3000 us
    CHECK(line.find("queue p50=n/a p95=n/a") != std::string::npos);
    CHECK(line.find("sleep p50=0.6 p95=0.6 (DLSS-G)") != std::string::npos); // the Reflex sleep inside the present, 500 us
    CHECK(line.find("age p5=4.1 p50=4.1 p95=4.1") != std::string::npos); // tick start 10.002 s to present start 10.006 s
    CHECK(line.find("same |") != std::string::npos);
    ext::latency::SetEventSink(&g_recorder);
}

// ---- source pins ----
std::string ReadFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string Between(const std::string& text, const std::string& from, const std::string& to)
{
    const std::size_t at = text.find(from);
    if (at == std::string::npos)
        return {};
    const std::size_t end = text.find(to, at + from.size());
    return text.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

void TestSourcePins(const std::string& repo)
{
    const std::string source = ReadFile(repo + "/OptiScaler/ext/latency/TickProbe.cpp");
    CHECK(!source.empty());
    const std::string present = Between(source, "void OnBridgePresentBegin()", "void OnBridgePresentEnd()");
    CHECK(!present.empty());
    // the key is the first thing read, before anything else, and it is the mirror (no ini read on the present path)
    CHECK(present.rfind("{\n    //", 0) != std::string::npos || present.find("if (!g_keyOn.load") != std::string::npos);
    const std::size_t keyGate = present.find("g_keyOn.load");
    CHECK(keyGate != std::string::npos && keyGate < present.find("ComputeArmed()"));
    CHECK(present.find("Cfg()") == std::string::npos);
    // no locked exchange on the arming path: the state changes only when it changes
    CHECK(source.find("g_armed.exchange") == std::string::npos);
    CHECK(present.find("g_armed.store") != std::string::npos);
    // the key is read from the config only by the mirror
    const std::string mirror = Between(source, "void RefreshKeys()", "void OnBridgePresentBegin()");
    CHECK(mirror.find("Cfg().framePacing.TickProbeOn()") != std::string::npos);
    // arming takes the lease, so the probe cannot start before the observer is in the slot
    const std::string armed = Between(source, "bool ComputeArmed()", "void OnKeysLoaded()");
    CHECK(armed.find("Leased()") != std::string::npos);
    // both ends of the Reflex sleep are tapped by the frame-pacing scope, which is what gives the sleep its own duration
    const std::string pacing = ReadFile(repo + "/OptiScaler/ext/present/FramePacingTrace.cpp");
    CHECK(!pacing.empty());
    CHECK(pacing.find("ext::latency::OnSleepScopeBegin();") != std::string::npos);
    CHECK(pacing.find("ext::latency::OnSleepScopeEnd();") != std::string::npos);
}
} // namespace

// The product stubs: the fake clock, the config key, the load hook and the two lease reads the probe taps make.
BOOL TestQueryPerformanceCounter(LARGE_INTEGER* value)
{
    value->QuadPart = g_nowUs;
    return TRUE;
}
BOOL TestQueryPerformanceFrequency(LARGE_INTEGER* value)
{
    value->QuadPart = 1'000'000; // one QPC unit is one microsecond
    return TRUE;
}
void ext::ExtConfigProbeStub::PostLoad()
{
    for (std::size_t i = 0; i < g_hookCount; ++i)
    {
        if (g_hooks[i] != nullptr)
            g_hooks[i]();
    }
}
ext::ExtConfigProbeStub& ext::Cfg() { return Config(); }
void ext::AddKeysLoadedHook(ext::KeysLoadedHook hook)
{
    if (hook != nullptr && g_hookCount < ext::kMaxKeysLoadedHooks)
        g_hooks[g_hookCount++] = hook;
}
namespace ext::latency
{
bool Leased() { return g_leased; }
bool ReadFrameCounter(std::uint32_t& value)
{
    if (!g_counterAvailable)
        return false;
    value = g_counterValue;
    return true;
}
} // namespace ext::latency

int main(int argc, char** argv)
{
    Active(); // publish once, before the product's static registration and the first tap
    ext::latency::SetEventSink(&g_recorder);
    TestKeyOffCostsOneLoad();
    TestArmingNeedsTheLease();
    TestEventsCarryTheCounter();
    TestNoteIsDeduplicated();
    TestWindowLineThroughTheLogSink();
    TestSameThreadWindowLineThroughTheLogSink();
    if (argc > 1)
        TestSourcePins(argv[1]);
    else
        std::printf("SKIP: no repo argument, source pins not checked\n");
    ext::latency::SetEventSink(nullptr);
    return TEST_RESULT("tick_probe_product");
}
