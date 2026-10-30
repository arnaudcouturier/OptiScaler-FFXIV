// The failure latch clears when the key arms. A start can fail after the key has already gone off - the
// worker's last write hits a full disk or loses its share, after the key-off clear had run, so the latch stays
// set - and the first tap of the next session then returned before it could retry: the menu showed the old
// error and no file appeared until the key had been toggled twice.
//
// The taps are driven here in the order a player produces them (key off, taps until disarmed, the late
// failure, key on, tap), never by calling the start directly: the tap path is the one that stopped working, and
// a key-off tap that runs before the late failure clears the latch on its own and would hide the case.
//
// This binary includes the seams and the core: the seams test links the core and cannot reach the latch.
#include "fgrec_json.h"

#include "ext/fgrec/FgRecSeams.cpp" // first: its unqualified State reads belong to the process stand-in

#include "ext/fgrec/FgRecorder.cpp"

#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace fg = ext::fgrec;
namespace ft = fgrec_test;

namespace ext::core
{
const char* ForkVersion() { return "fgrec-rearm"; }
} // namespace ext::core

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

// The session file of the current (or last) session, as the start named it.
static std::wstring SessionBase()
{
    std::lock_guard<std::mutex> lock(fg::Global().mutex);
    return fg::Global().file;
}

// A skipped present still runs the whole tap (key poll and start), and emits nothing.
static void Tap()
{
    fg::FgPresentBegin(false, 0, 0);
}

static bool TapUntilAlive()
{
    for (int i = 0; i < 400; ++i)
    {
        if (fg::Alive())
            return true;
        Tap(); // the key poll runs every 16th tap per thread
        Sleep(2);
    }
    return fg::Alive();
}

static void TapUntilDisarmed()
{
    for (int i = 0; i < 400 && fg::RecArmed(); ++i)
    {
        Tap();
        Sleep(1);
    }
}

static void LateFailureClearsWhenTheKeyArms()
{
    ext::Cfg().framePacing.fgRecorder = true; // key on: the first taps arm and start the session
    CHECK(TapUntilAlive());
    fg::Emit(fg::kSleep, 1, 10, 0, 0, "first");
    const std::wstring firstSession = SessionBase();
    CHECK(!firstSession.empty());

    ext::Cfg().framePacing.fgRecorder = false; // key off: the worker drains, ends and exits by itself
    TapUntilDisarmed();
    CHECK(!fg::RecArmed());
    CHECK(ft::WaitUntil(WorkerGone, 30000));
    CHECK(ft::WaitForSessionEnd(firstSession, 30000));

    // The worker's last write failed after the key-off clear had run (or lost its try_lock). The latch is set
    // the way that failure path leaves it: under the mutex, with the error code the menu shows.
    {
        std::lock_guard<std::mutex> lock(fg::Global().mutex);
        fg::Global().error.store(2, std::memory_order_relaxed);
        fg::Global().failed.store(true, std::memory_order_relaxed);
    }
    CHECK(fg::Failed());

    ext::Cfg().framePacing.fgRecorder = true; // key on again: the first tap of the session must retry
    const bool alive = TapUntilAlive();
    CHECK(alive);               // a latch that survives the arm leaves nothing running
    CHECK(fg::Error() == 0);    // and the old error code on the menu
    fg::Emit(fg::kSleep, 2, 20, 0, 0, "second");
    const std::wstring secondSession = SessionBase();
    CHECK(!secondSession.empty());
    CHECK(secondSession != firstSession); // a new file, not the old session's

    ext::Cfg().framePacing.fgRecorder = false;
    TapUntilDisarmed();
    CHECK(ft::WaitUntil(WorkerGone, 30000));
    CHECK(ft::WaitForSessionEnd(secondSession, 30000));
    const std::string bytes = ft::ReadBytes(secondSession);
    CHECK(bytes.find("second") != std::string::npos); // the key-on record is in the new session
    std::printf("second session lines: %zu\n", ft::SplitLines(bytes).size());
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    LateFailureClearsWhenTheKeyArms();
    ext::Cfg().framePacing.fgRecorder = false;
    fg::EnsureStarted(false, fg::SessionInfo {});
    if (!ft::WaitUntil(WorkerGone, 30000))
        std::printf("the worker did not stop\n");
    if (g_failed != 0)
    {
        std::printf("fgrec_rearm: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("fgrec_rearm: all passed\n");
    return 0;
}
