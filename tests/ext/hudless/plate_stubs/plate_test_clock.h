#pragma once
// The clock the plate engine measures its windows on, for the check build only. Production reads GetTickCount64 through
// PLATES_NOW(); the engine recipe defines PLATES_NOW to this clock instead, so a scenario can drive the 250 ms record
// window and the 100 ms alive window at their real values rather than widening them.
//
// Header only and shared by the engine and the harness (one inline variable), which is why the tick lives here. The tick
// is atomic: a scenario that runs the game side and the presenting side on two threads advances it from both.
#include <atomic>

namespace plate_test_clock
{
inline std::atomic<unsigned long long> g_now { 1000 }; // a nonzero base: a zero tick means "never" to the engine

inline unsigned long long Now()
{
    return g_now.load(std::memory_order_relaxed);
}

// One frame or one output of a scenario: a 100 Hz-ish cadence, far inside both windows.
inline void Advance(unsigned long long milliseconds = 8)
{
    g_now.fetch_add(milliseconds, std::memory_order_relaxed);
}

inline void Set(unsigned long long tick)
{
    g_now.store(tick, std::memory_order_relaxed);
}

inline void Reset()
{
    g_now.store(1000, std::memory_order_relaxed);
}
} // namespace plate_test_clock
