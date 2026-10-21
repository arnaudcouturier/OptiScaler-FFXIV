// [QUICK-QR] The install state machine of the native-quality hooks (ext/ffxiv/quality/NativeQualityRetry.h): what the one-shot
// installer did before (install once, cache, one step at a time, never again after a deterministic refusal, fail open on a
// throw) and what the retry adds (up to 3 attempts, 250 ms then 2000 ms apart, the Detours error table, one log line per
// episode). Pure CPU, no Windows, no Detours: the step, the clock and the log are injected.
#include "ext/ffxiv/quality/NativeQualityRetry.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string_view>
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
    int throwOnCall = -1;            // the step throws at this (0-based) call
    bool logThrows = false;          // every log callback throws

    std::atomic<uint64_t> now { 1000 };
    std::atomic<int> clockReads { 0 };
    std::atomic<uint64_t> callTime[8] {}; // the clock when call n started

    std::atomic<int> failedLines { 0 };
    std::atomic<int> gaveUpLines { 0 };
    std::atomic<int> recoveredLines { 0 };
    std::atomic<int> lastFailedAttempt { 0 };
    std::atomic<int> lastGaveUpAttempt { 0 };
    std::atomic<int> lastRecoveredAttempt { 0 };
    std::atomic<int> lastMaxAttempts { 0 };
    std::atomic<long> lastGaveUpError { 0 };

    StepResult Run()
    {
        const int now_running = ++running;
        int seen = maxRunning.load();
        while (now_running > seen && !maxRunning.compare_exchange_weak(seen, now_running))
        {
        }
        inStep.store(true);
        while (!gate.load())
            std::this_thread::yield();
        const int call = calls.fetch_add(1);
        if (call < 8)
            callTime[call].store(now.load());
        if (call == throwOnCall)
        {
            --running;
            throw std::runtime_error("step threw");
        }
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
    void Failed(int attempt, int maxAttempts, const StepResult&) const
    {
        rig->failedLines.fetch_add(1);
        rig->lastFailedAttempt.store(attempt);
        rig->lastMaxAttempts.store(maxAttempts);
        if (rig->logThrows)
            throw std::runtime_error("log threw");
    }
    void GaveUp(int attempt, int, const StepResult& result) const
    {
        rig->gaveUpLines.fetch_add(1);
        rig->lastGaveUpAttempt.store(attempt);
        rig->lastGaveUpError.store(result.error);
        if (rig->logThrows)
            throw std::runtime_error("log threw");
    }
    void Recovered(int attempt) const
    {
        rig->recoveredLines.fetch_add(1);
        rig->lastRecoveredAttempt.store(attempt);
        if (rig->logThrows)
            throw std::runtime_error("log threw");
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
    CHECK(rig.clockReads.load() == 0); // a first attempt that succeeds never needs the time
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
    {
        Rig rig; // gave up after three transient failures
        rig.script = { Transient() };
        MAKE_INSTALLER(installer, rig);
        CHECK(!installer.Ensure());
        rig.now += 250;
        CHECK(!installer.Ensure());
        rig.now += 2000;
        CHECK(!installer.Ensure());
        CHECK(rig.calls.load() == 3);
        rig.clockReads = 0;
        for (int i = 0; i < 10000; ++i)
            CHECK(!installer.Ensure());
        CHECK(rig.clockReads.load() == 0);
    }
}

static void AThrowingStepFailsOpenAndIsNotRetried()
{
    Rig rig;
    rig.throwOnCall = 0;
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
    rig.throwOnCall = -1; // a retry would now succeed: it must not be tried
    rig.now += 10 * 60 * 1000;
    CHECK(!installer.Ensure());
    CHECK(rig.calls.load() == 1);
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

// ---- The retry -----------------------------------------------------------------------------------------------------------

static void ATransientFailureIsRetriedAfter250msAndTheSecondAttemptInstalls()
{
    Rig rig;
    rig.script = { Transient(), Ok() };
    MAKE_INSTALLER(installer, rig);
    CHECK(!installer.Ensure()); // attempt 1 at t = 1000
    CHECK(rig.calls.load() == 1);
    CHECK(rig.failedLines.load() == 1 && rig.lastFailedAttempt.load() == 1 && rig.lastMaxAttempts.load() == 3);

    for (const uint64_t at : { 1000ull, 1001ull, 1100ull, 1249ull }) // before the due time (1000 + 250)
    {
        rig.now = at;
        CHECK(!installer.Ensure());
        CHECK(rig.calls.load() == 1);
    }
    rig.now = 1250; // exactly due
    CHECK(installer.Ensure());
    CHECK(rig.calls.load() == 2);
    CHECK(rig.recoveredLines.load() == 1 && rig.lastRecoveredAttempt.load() == 2);
    CHECK(rig.failedLines.load() == 1 && rig.gaveUpLines.load() == 0);
    CHECK(installer.Ensure()); // and it stays installed
    CHECK(rig.calls.load() == 2);
}

static void TheSecondGapIs2000msAndTheThirdAttemptCanStillInstall()
{
    Rig rig;
    rig.script = { Transient(), Transient(), Ok() };
    MAKE_INSTALLER(installer, rig);
    CHECK(!installer.Ensure()); // attempt 1 at 1000, next due 1250
    rig.now = 1250;
    CHECK(!installer.Ensure()); // attempt 2 at 1250, next due 3250
    CHECK(rig.calls.load() == 2);
    CHECK(rig.failedLines.load() == 2 && rig.lastFailedAttempt.load() == 2);

    for (const uint64_t at : { 1251ull, 1500ull, 2999ull, 3249ull }) // the short gap does not apply twice
    {
        rig.now = at;
        CHECK(!installer.Ensure());
        CHECK(rig.calls.load() == 2);
    }
    rig.now = 3250;
    CHECK(installer.Ensure());
    CHECK(rig.calls.load() == 3);
    CHECK(rig.recoveredLines.load() == 1 && rig.lastRecoveredAttempt.load() == 3);
    CHECK(rig.gaveUpLines.load() == 0);
}

static void ThreeTransientFailuresEndInOffForever()
{
    Rig rig;
    rig.script = { Transient(Stage::Commit, 31337) };
    MAKE_INSTALLER(installer, rig);
    CHECK(!installer.Ensure());
    rig.now += 250;
    CHECK(!installer.Ensure());
    rig.now += 2000;
    CHECK(!installer.Ensure());
    CHECK(rig.calls.load() == 3);
    CHECK(rig.failedLines.load() == 2); // attempts 1 and 2 said "retrying"
    CHECK(rig.gaveUpLines.load() == 1 && rig.lastGaveUpAttempt.load() == 3 && rig.lastGaveUpError.load() == 31337);

    rig.script = { Ok() }; // a fourth attempt would install: it must never be made
    for (int i = 0; i < 1000; ++i)
    {
        rig.now += 1000 * 60 * 60;
        CHECK(!installer.Ensure());
    }
    CHECK(rig.calls.load() == 3);
    CHECK(rig.gaveUpLines.load() == 1); // once per episode, not once per poll
    CHECK(rig.failedLines.load() == 2);
}

static void APermanentFailureMidSequenceStopsTheRetry()
{
    Rig rig;
    rig.script = { Transient(), Permanent(Stage::AttachQuerySize), Ok() };
    MAKE_INSTALLER(installer, rig);
    CHECK(!installer.Ensure());
    rig.now += 250;
    CHECK(!installer.Ensure());
    CHECK(rig.calls.load() == 2);
    CHECK(rig.gaveUpLines.load() == 1 && rig.lastGaveUpAttempt.load() == 2);
    CHECK(rig.failedLines.load() == 1);
    rig.now += 100000;
    CHECK(!installer.Ensure());
    CHECK(rig.calls.load() == 2);
}

static void AGuardRefusalKeepsItsOwnSingleLineAndAddsNone()
{
    Rig rig;
    rig.script = { Permanent(Stage::Guard) }; // the step already logged its warning, exactly as before
    MAKE_INSTALLER(installer, rig);
    CHECK(!installer.Ensure());
    CHECK(rig.failedLines.load() == 0 && rig.gaveUpLines.load() == 0 && rig.recoveredLines.load() == 0);
}

static void PollingWhileWaitingNeitherRunsTheStepNorRepeatsTheLine()
{
    Rig rig;
    rig.script = { Transient(), Ok() };
    MAKE_INSTALLER(installer, rig);
    CHECK(!installer.Ensure());
    for (int i = 0; i < 5000; ++i)
        CHECK(!installer.Ensure()); // the clock still reads 1000: not due
    CHECK(rig.calls.load() == 1);
    CHECK(rig.failedLines.load() == 1);
}

static void AThrowDuringARetryEndsInOffAndIsLoggedOnce()
{
    Rig rig;
    rig.script = { Transient() };
    rig.throwOnCall = 1;
    MAKE_INSTALLER(installer, rig);
    CHECK(!installer.Ensure());
    rig.now += 250;
    bool threw = false;
    try
    {
        CHECK(!installer.Ensure());
    }
    catch (...)
    {
        threw = true;
    }
    CHECK(!threw);
    CHECK(rig.calls.load() == 2);
    CHECK(rig.gaveUpLines.load() == 1 && rig.lastGaveUpAttempt.load() == 2);
    rig.throwOnCall = -1;
    rig.now += 100000;
    CHECK(!installer.Ensure());
    CHECK(rig.calls.load() == 2);
}

static void ALogThatThrowsNeverEscapesAndNeverBlocksTheRetry()
{
    Rig rig;
    rig.script = { Transient(), Ok() };
    rig.logThrows = true;
    MAKE_INSTALLER(installer, rig);
    bool threw = false;
    bool first = true;
    bool second = false;
    try
    {
        first = installer.Ensure();
        rig.now += 250;
        second = installer.Ensure();
    }
    catch (...)
    {
        threw = true;
    }
    CHECK(!threw);
    CHECK(!first);
    CHECK(second);
    CHECK(rig.calls.load() == 2);
}

static void AFailureWhileSevenOthersWaitDoesNotLetThemRunTheNextAttempt()
{
    Rig rig;
    rig.gate = false;
    rig.script = { Transient(), Ok() };
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
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // seven are queued behind the first attempt
    rig.gate = true;
    for (auto& thread : threads)
        thread.join();
    // The clock never moved: attempt 2 is not due, so the waiters see "false" and nobody ran it early.
    CHECK(rig.calls.load() == 1);
    CHECK(trueSeen.load() == 0);
    CHECK(rig.maxRunning.load() == 1);
    CHECK(rig.failedLines.load() == 1);
}

static void EightPollingThreadsNeverOverlapAndKeepTheSpacing()
{
    Rig rig;
    rig.script = { Transient() }; // never installs
    MAKE_INSTALLER(installer, rig);
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t)
        threads.emplace_back(
            [&]
            {
                for (int i = 0; i < 20000; ++i)
                {
                    rig.now.fetch_add(40); // time passes while they poll
                    installer.Ensure();
                    if ((i & 63) == 0)
                        std::this_thread::yield();
                }
            });
    for (auto& thread : threads)
        thread.join();
    CHECK(rig.calls.load() == 3);
    CHECK(rig.maxRunning.load() == 1);
    CHECK(rig.callTime[1].load() >= rig.callTime[0].load() + 250);
    CHECK(rig.callTime[2].load() >= rig.callTime[1].load() + 2000);
    CHECK(rig.gaveUpLines.load() == 1 && rig.failedLines.load() == 2);
}

static void TheDetoursErrorTable()
{
    CHECK(ClassifyDetours(Stage::Begin, 4317) == Outcome::Transient);            // ERROR_INVALID_OPERATION: another transaction is open
    CHECK(ClassifyDetours(Stage::EnlistCurrent, 5) == Outcome::Transient);
    CHECK(ClassifyDetours(Stage::EnlistThread, 5) == Outcome::Transient);        // access denied: the thread is exiting
    CHECK(ClassifyDetours(Stage::EnlistThread, 8) == Outcome::Transient);        // out of memory
    CHECK(ClassifyDetours(Stage::ThreadSnapshot, 5) == Outcome::Transient);
    CHECK(ClassifyDetours(Stage::Commit, 5) == Outcome::Transient);
    CHECK(ClassifyDetours(Stage::AttachQuerySize, 4317) == Outcome::Transient);
    CHECK(ClassifyDetours(Stage::AttachRendererUpdate, 8) == Outcome::Transient);
    CHECK(ClassifyDetours(Stage::EnlistThread, 99999) == Outcome::Transient);    // unknown codes are retried
    CHECK(ClassifyDetours(Stage::AttachQuerySize, 99999) == Outcome::Transient);
    CHECK(ClassifyDetours(Stage::AttachQuerySize, 9) == Outcome::Permanent);     // ERROR_INVALID_BLOCK at an attach: deterministic
    CHECK(ClassifyDetours(Stage::AttachRendererUpdate, 9) == Outcome::Permanent);
    CHECK(ClassifyDetours(Stage::AttachQuerySize, 6) == Outcome::Permanent);     // ERROR_INVALID_HANDLE at an attach
    CHECK(ClassifyDetours(Stage::AttachRendererUpdate, 6) == Outcome::Permanent);
    CHECK(ClassifyDetours(Stage::EnlistThread, 9) == Outcome::Transient);        // the same codes elsewhere mean something else
    CHECK(ClassifyDetours(Stage::Commit, 6) == Outcome::Transient);
    CHECK(ClassifyDetours(Stage::Begin, 6) == Outcome::Transient);
    CHECK(ClassifyDetours(Stage::Begin, 0) == Outcome::Installed);
    CHECK(ClassifyDetours(Stage::None, 0) == Outcome::Installed);
}

static void TheDelaysAndTheCap()
{
    CHECK(kMaxAttempts == 3);
    CHECK(RetryDelayMs(1) == 250);
    CHECK(RetryDelayMs(2) == 2000);
}

static void StageNamesAreDistinctAndNonEmpty()
{
    const Stage all[] = { Stage::None,          Stage::Guard,           Stage::ThreadSnapshot, Stage::Begin,
                          Stage::EnlistCurrent, Stage::EnlistThread,    Stage::AttachQuerySize,
                          Stage::AttachRendererUpdate, Stage::Commit,   Stage::Exception };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i)
    {
        CHECK(StageName(all[i])[0] != '\0');
        for (size_t j = i + 1; j < sizeof(all) / sizeof(all[0]); ++j)
            CHECK(std::string_view(StageName(all[i])) != std::string_view(StageName(all[j])));
    }
}

int main()
{
    SuccessIsCachedAndRunsOnceUnderContention();
    PermanentRunsOnceAndNeverAgain();
    TerminalStatesNeverReadTheClock();
    AThrowingStepFailsOpenAndIsNotRetried();
    WaitersGetTheResultOfTheOneInstallAndNeverRunTheStepAgain();

    ATransientFailureIsRetriedAfter250msAndTheSecondAttemptInstalls();
    TheSecondGapIs2000msAndTheThirdAttemptCanStillInstall();
    ThreeTransientFailuresEndInOffForever();
    APermanentFailureMidSequenceStopsTheRetry();
    AGuardRefusalKeepsItsOwnSingleLineAndAddsNone();
    PollingWhileWaitingNeitherRunsTheStepNorRepeatsTheLine();
    AThrowDuringARetryEndsInOffAndIsLoggedOnce();
    ALogThatThrowsNeverEscapesAndNeverBlocksTheRetry();
    AFailureWhileSevenOthersWaitDoesNotLetThemRunTheNextAttempt();
    EightPollingThreadsNeverOverlapAndKeepTheSpacing();
    TheDetoursErrorTable();
    TheDelaysAndTheCap();
    StageNamesAreDistinctAndNonEmpty();

    if (g_failed != 0)
    {
        std::printf("native_quality_retry: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("native_quality_retry: all checks passed\n");
    return 0;
}
