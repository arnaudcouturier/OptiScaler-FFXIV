// [REL] The thread-id lines (ext/upscaler/NgxThreadTraceCore.h), pure CPU.
//
// They exist so the log shows afterwards which threads create, evaluate and release the DX11 upscaler, i.e. whether the narrow
// table lock and the closed/in-flight handling were ever needed. The format is pinned here because the in-game check greps it:
//   FFXIV NGX thread: create tid=<n> render=<w>x<h> display=<w>x<h>
//   FFXIV NGX thread: evaluate key=<hex> tid=<n> first
//   FFXIV NGX thread: evaluate key=<hex> tid=<n> moved from=<n>
//   FFXIV NGX thread: evaluate key=<hex> tid=<n> evaluations=<n> in the last <ms> ms
//   FFXIV NGX thread: release handle=<n> tid=<n>
#include "ext/upscaler/NgxThreadTraceCore.h"

#include <cstdint>
#include <cstdio>
#include <string>

using namespace ext::upscaler::threadtrace;

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

int main()
{
    // ---- pinned formats ------------------------------------------------------------------------------------------------------
    CHECK(FormatCreate(4242, 2258, 1270, 3840, 2160) == "FFXIV NGX thread: create tid=4242 render=2258x1270 display=3840x2160");
    CHECK(FormatRelease(1000001, 4242) == "FFXIV NGX thread: release handle=1000001 tid=4242");
    {
        Decision first;
        first.log = true;
        first.reason = Reason::First;
        CHECK(FormatEvaluate(0x1F2A, 99, first) == "FFXIV NGX thread: evaluate key=1f2a tid=99 first");
        Decision moved;
        moved.log = true;
        moved.reason = Reason::Moved;
        moved.previousTid = 77;
        CHECK(FormatEvaluate(0x1F2A, 99, moved) == "FFXIV NGX thread: evaluate key=1f2a tid=99 moved from=77");
        Decision periodic;
        periodic.log = true;
        periodic.reason = Reason::Periodic;
        periodic.count = 600;
        periodic.intervalMs = 10000;
        CHECK(FormatEvaluate(0x1F2A, 99, periodic) == "FFXIV NGX thread: evaluate key=1f2a tid=99 evaluations=600 in the last 10000 ms");
    }

    // ---- evaluate tracker: first call logs, steady state is silent, a thread change logs, then one line per interval ------------
    {
        EvaluateTracker t(10000);
        auto d = t.OnEvaluate(1, 100, 0);
        CHECK(d.log && d.reason == Reason::First);
        for (int i = 1; i <= 599; ++i)
        {
            d = t.OnEvaluate(1, 100, static_cast<uint64_t>(i) * 16);
            CHECK(!d.log); // same thread, inside the interval: silent
        }
        d = t.OnEvaluate(1, 101, 9600); // another thread
        CHECK(d.log && d.reason == Reason::Moved && d.previousTid == 100);
        d = t.OnEvaluate(1, 101, 9700);
        CHECK(!d.log);
        // the interval is counted from the last line that was written
        d = t.OnEvaluate(1, 101, 9600 + 10000);
        CHECK(d.log && d.reason == Reason::Periodic);
        CHECK(d.count == 2); // the calls since the previous line, this one included: 9700 and 19600
        CHECK(d.intervalMs == 10000);
        d = t.OnEvaluate(1, 101, 19600 + 5);
        CHECK(!d.log);
    }
    // two keys are tracked independently (the scene and a character preview evaluate on different contexts)
    {
        EvaluateTracker t(10000);
        CHECK(t.OnEvaluate(1, 100, 0).log);
        CHECK(t.OnEvaluate(2, 200, 1).log);
        CHECK(!t.OnEvaluate(1, 100, 2).log);
        CHECK(!t.OnEvaluate(2, 200, 3).log);
        const auto d = t.OnEvaluate(2, 201, 4);
        CHECK(d.log && d.reason == Reason::Moved && d.previousTid == 200);
        CHECK(!t.OnEvaluate(1, 100, 5).log); // key 1 is unaffected
    }
    // bounded state: far more keys than slots never grows without limit and never crashes; an evicted key logs as first again
    {
        EvaluateTracker t(10000);
        for (uintptr_t k = 1; k <= 64; ++k)
            t.OnEvaluate(k, 1, k);
        CHECK(t.TrackedKeys() <= EvaluateTracker::kMaxKeys);
        CHECK(t.OnEvaluate(65, 1, 100).log); // a new key still logs
    }
    // time going backwards (a clock reset) never produces a line per call
    {
        EvaluateTracker t(10000);
        t.OnEvaluate(1, 100, 50000);
        CHECK(!t.OnEvaluate(1, 100, 10).log);
        CHECK(!t.OnEvaluate(1, 100, 11).log);
    }

    if (g_failed == 0)
        std::printf("ngx_thread_trace: all checks passed\n");
    return g_failed;
}
