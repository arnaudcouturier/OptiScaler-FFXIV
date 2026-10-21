// [QUICK-QR] The install state machine of the native-quality hooks (ext/ffxiv/quality/NativeQualityRetry.h): what the one-shot
// installer did before (install once, cache, one step at a time, never again after a deterministic refusal, fail open on a
// throw) and what the retry adds. Pure CPU, no Windows, no Detours: the step, the clock and the log are injected.
#include "ext/ffxiv/quality/NativeQualityRetry.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace ext::ffxiv::quality::retry;

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

static StepResult Ok() { return { Outcome::Installed, Stage::None, 0, 0 }; }
static StepResult Transient(Stage stage = Stage::EnlistThread, long error = 5, unsigned thread = 77)
{
    return { Outcome::Transient, stage, error, thread };
}
static StepResult Permanent(Stage stage = Stage::Guard) { return { Outcome::Permanent, stage, 0, 0 }; }

// Everything the injected step, clock and log record. One rig per test.
struct Rig
{
    std::vector<StepResult> script;  // result of call n; the last entry repeats; empty = always installed
    std::atomic<int> calls { 0 };
    std::atomic<int> running { 0 };
    std::atomic<int> maxRunning { 0 };
    std::atomic<bool> gate { true }; // false: the step blocks until it is set
    std::atomic<bool> inStep { false };
    bool throwOnStep = false;

    std::atomic<uint64_t> now { 1000 };
    std::atomic<int> clockReads { 0 };

    std::atomic<int> failedLines { 0 };
    std::atomic<int> gaveUpLines { 0 };
    std::atomic<int> recoveredLines { 0 };
    std::atomic<int> lastFailedAttempt { 0 };
    std::atomic<int> lastGaveUpAttempt { 0 };
    std::atomic<int> lastRecoveredAttempt { 0 };

    StepResult Run()
    {
        const int now = ++running;
        int seen = maxRunning.load();
        while (now > seen && !maxRunning.compare_exchange_weak(seen, now))
        {
        }
        inStep.store(true);
        while (!gate.load())
            std::this_thread::yield();
        if (throwOnStep)
        {
            --running;
            throw std::runtime_error("step threw");
        }
        const int call = calls.fetch_add(1);
        StepResult result = Ok();
        if (!script.empty())
            result = script[static_cast<size_t>(call) < script.size() ? static_cast<size_t>(call) : script.size() - 1];
        --running;
        return result;
    }
};

struct StepFn
{
    Rig* rig;
    StepResult operator()() const { return rig->Run(); }
};
struct ClockFn
{
    Rig* rig;
    uint64_t operator()() const
    {
        rig->clockReads.fetch_add(1);
        return rig->now.load();
    }
};
struct LogFn
{
    Rig* rig;
    void Failed(int attempt, int, const StepResult&) const
    {
        rig->failedLines.fetch_add(1);
        rig->lastFailedAttempt.store(attempt);
    }
    void GaveUp(int attempt, int, const StepResult&) const
    {
        rig->gaveUpLines.fetch_add(1);
        rig->lastGaveUpAttempt.store(attempt);
    }
    void Recovered(int attempt) const
    {
        rig->recoveredLines.fetch_add(1);
        rig->lastRecoveredAttempt.store(attempt);
    }
};
using Installer = RetryingInstaller<StepFn, ClockFn, LogFn>;

#define MAKE_INSTALLER(name, rig) Installer name { StepFn { &(rig) }, ClockFn { &(rig) }, LogFn { &(rig) } }

static void WaitForStep(Rig& rig)
{
    while (!rig.inStep.load())
        std::this_thread::yield();
}

// ---- What must not change (these pass against the one-shot installer too) ------------------------------------------------

static void SuccessIsCachedAndRunsOnceUnderContention()
{
    Rig rig;
    MAKE_INSTALLER(installer, rig);
    std::atomic<int> falseSeen { 0 };
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t)
        threads.emplace_back(
            [&]
            {
                for (int i = 0; i < 2000; ++i)
                    if (!installer.Ensure())
                        falseSeen.fetch_add(1);
            });
    for (auto& thread : threads)
        thread.join();
    CHECK(rig.calls.load() == 1);
    CHECK(falseSeen.load() == 0);
    CHECK(rig.maxRunning.load() == 1);
    CHECK(rig.failedLines.load() == 0 && rig.gaveUpLines.load() == 0 && rig.recoveredLines.load() == 0);
}

static void PermanentRunsOnceAndNeverAgain()
{
    Rig rig;
    rig.script = { Permanent(Stage::Guard), Ok() }; // a second call would "succeed": it must never happen
    MAKE_INSTALLER(installer, rig);
    CHECK(!installer.Ensure());
    CHECK(rig.calls.load() == 1);
    rig.now += 10 * 60 * 1000;
    for (int i = 0; i < 50; ++i)
        CHECK(!installer.Ensure());
    CHECK(rig.calls.load() == 1);
}

static void TerminalStatesNeverReadTheClock()
{
    {
        Rig rig;
        MAKE_INSTALLER(installer, rig);
        CHECK(installer.Ensure());
        rig.clockReads = 0;
        for (int i = 0; i < 10000; ++i)
            CHECK(installer.Ensure());
        CHECK(rig.clockReads.load() == 0);
    }
    {
        Rig rig;
        rig.script = { Permanent() };
        MAKE_INSTALLER(installer, rig);
        CHECK(!installer.Ensure());
        rig.clockReads = 0;
        for (int i = 0; i < 10000; ++i)
            CHECK(!installer.Ensure());
        CHECK(rig.clockReads.load() == 0);
    }
}

static void AThrowingStepFailsOpenAndIsNotRetried()
{
    Rig rig;
    rig.throwOnStep = true;
    MAKE_INSTALLER(installer, rig);
    bool threw = false;
    bool result = true;
    try
    {
        result = installer.Ensure();
    }
    catch (...)
    {
        threw = true;
    }
    CHECK(!threw);
    CHECK(!result);
    rig.throwOnStep = false; // a retry would now succeed: it must not be tried
    rig.now += 10 * 60 * 1000;
    CHECK(!installer.Ensure());
    CHECK(rig.calls.load() == 0);
}

static void WaitersGetTheResultOfTheOneInstallAndNeverRunTheStepAgain()
{
    Rig rig;
    rig.gate = false; // the first caller blocks inside the step
    MAKE_INSTALLER(installer, rig);
    std::atomic<int> entered { 0 };
    std::atomic<int> trueSeen { 0 };
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t)
        threads.emplace_back(
            [&]
            {
                entered.fetch_add(1);
                if (installer.Ensure())
                    trueSeen.fetch_add(1);
            });
    WaitForStep(rig);
    while (entered.load() < 8)
        std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // let the other seven reach the lock
    rig.gate = true;
    for (auto& thread : threads)
        thread.join();
    CHECK(rig.calls.load() == 1);
    CHECK(rig.maxRunning.load() == 1);
    CHECK(trueSeen.load() == 8);
}

int main()
{
    SuccessIsCachedAndRunsOnceUnderContention();
    PermanentRunsOnceAndNeverAgain();
    TerminalStatesNeverReadTheClock();
    AThrowingStepFailsOpenAndIsNotRetried();
    WaitersGetTheResultOfTheOneInstallAndNeverRunTheStepAgain();

    if (g_failed != 0)
    {
        std::printf("native_quality_retry: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("native_quality_retry: all checks passed\n");
    return 0;
}
