// The writer's cadence. The old tap woke the writer on every empty-to-non-empty queue transition, which at
// 120 fps is once per frame, and every wake with a batch flushed the device: 73 flushes a second against the
// documented "about once a second", each a cache flush that stalls the game's own disk I/O for no benefit (data
// already written survives a process crash without one). The fix wakes the writer on its own timer or when the
// queue passes its high-water mark, and flushes at most about once a second.
//
// This test runs a real session with the proof's load (120 fps x 6 taps) and reads loopCount and flushCount,
// the two counters the writer keeps. The counters are rates here, so the five-second window stands in for the
// same sixty-second run (the outside proof repeats it at full length). A wake per batch shows up as loops
// near the tap rate and flushes near the frame rate; both limits below are far above the timer cadence.
#include "fgrec_json.h"

#include "ext/fgrec/FgRecorder.cpp"

#include <chrono>
#include <cstdio>
#include <mmsystem.h>
#include <mutex>
#include <string>
#include <vector>

namespace fg = ext::fgrec;
namespace ft = fgrec_test;

static int g_failed = 0;
#define CHECK(cond)                                                                                              \
    do                                                                                                           \
    {                                                                                                            \
        if (!(cond))                                                                                             \
        {                                                                                                        \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                                                  \
            ++g_failed;                                                                                          \
        }                                                                                                        \
    } while (0)

static bool WorkerGone()
{
    std::lock_guard<std::mutex> lock(fg::Global().mutex);
    return !fg::Global().running;
}

static void CadencePins(const std::string& repo)
{
    std::string text;
    FILE* f = nullptr;
    const std::string path = repo + "/OptiScaler/ext/fgrec/FgRecorder.cpp";
    if (fopen_s(&f, path.c_str(), "rb") != 0 || f == nullptr)
    {
        std::printf("FAIL: cannot read %s\n", path.c_str());
        ++g_failed;
        return;
    }
    char buf[65536] = {};
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof buf, f)) != 0)
        text.append(buf, n);
    fclose(f);
    // The tap wakes the writer only past the high-water mark, never on an empty queue.
    CHECK(text.find("const bool wake = s.queue.size() >= kHighWater;") != std::string::npos);
    CHECK(text.find("const bool wake = s.queue.empty();") == std::string::npos);
    // The flush waits for its own cadence.
    CHECK(text.find("now - lastFlush >= kFlushMs") != std::string::npos);
    CHECK(text.find("constexpr uint64_t kFlushMs = 1000;") != std::string::npos);
}

static void WriterRanForThreeSeconds()
{
    const std::wstring dir = ft::FreshDir("flush");
    fg::SessionInfo info;
    const std::string v = "t", e = "t.exe", b = "t", sum = "t";
    info.version = v.c_str();
    info.exe = e.c_str();
    info.build = b.c_str();
    info.summary = sum.c_str();
    info.moduleDir = dir.c_str();
    fg::EnsureStarted(true, info);
    CHECK(fg::Running());
    // Settle: the session file exists and the worker has written its begin line.
    Sleep(300);
    const uint64_t loops0 = fg::Global().loopCount.load(std::memory_order_relaxed);
    const uint64_t flushes0 = fg::Global().flushCount.load(std::memory_order_relaxed);

    // 120 fps x 6 taps, real time: 360 frames over about three seconds, spaced
    // 8.3 ms apart like the frame they belong to (the 1 ms timer resolution is
    // what makes a sleep of 8 ms a sleep of 8 ms).
    timeBeginPeriod(1);
    const auto start = std::chrono::steady_clock::now();
    const int frames = 360;
    for (int frame = 0; frame < frames; ++frame)
    {
        for (int tap = 0; tap < 6; ++tap)
            fg::Emit(fg::kFgPresentBegin, static_cast<uint64_t>(frame), 1, 0, 0, nullptr);
        Sleep(8);
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    timeEndPeriod(1);
    const uint64_t loops = fg::Global().loopCount.load(std::memory_order_relaxed) - loops0;
    const uint64_t flushes = fg::Global().flushCount.load(std::memory_order_relaxed) - flushes0;
    const double perSecondLoops = seconds > 0.0 ? static_cast<double>(loops) / seconds : 0.0;
    const double perSecondFlushes = seconds > 0.0 ? static_cast<double>(flushes) / seconds : 0.0;
    std::printf("%d frames x 6 taps in %.2f s: loops %llu (%.1f/s), flushes %llu (%.2f/s)\n", frames, seconds,
                static_cast<unsigned long long>(loops), perSecondLoops, static_cast<unsigned long long>(flushes),
                perSecondFlushes);
    std::printf("60 s at this rate: loops %llu, flushes %llu\n",
                static_cast<unsigned long long>(perSecondLoops * 60.0 + 0.5),
                static_cast<unsigned long long>(perSecondFlushes * 60.0 + 0.5));
    CHECK(perSecondLoops <= 12.0); // the 100 ms timer, not one wake per tap batch
    CHECK(perSecondFlushes <= 1.2); // one device flush a second, not one per frame
    CHECK(loops >= 3); // the writer really ran (a silently dead worker fails here)
    CHECK(flushes >= 1); // and it did flush at least once

    fg::EnsureStarted(false, fg::SessionInfo {});
    CHECK(ft::WaitUntil(WorkerGone, 60000));
    CHECK(fg::Error() == 0);
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string repo = argc > 1 ? argv[1] : ".";
    CadencePins(repo);
    WriterRanForThreeSeconds();
    if (g_failed != 0)
    {
        std::printf("fgrec_flush: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("fgrec_flush: all passed\n");
    return 0;
}
