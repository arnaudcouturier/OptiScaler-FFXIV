#pragma once
// Shared by the dynamic multi-frame generation tests: a CHECK macro and a fake-clock driver for the selector.
#include "ext/fg/DynamicMfg.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <vector>

inline int g_failures = 0;
#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                                \
            ++g_failures;                                                                                              \
        }                                                                                                              \
    } while (0)
#define CHECK_MSG(cond, ...)                                                                                           \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::printf("FAIL %s:%d  %s  -- ", __FILE__, __LINE__, #cond);                                             \
            std::printf(__VA_ARGS__);                                                                                  \
            std::printf("\n");                                                                                         \
            ++g_failures;                                                                                              \
        }                                                                                                              \
    } while (0)

struct Switch
{
    uint64_t t;
    int from, to;
    ext::fg::DynamicMfg::Why why;
};

// Fake clock around one selector: every sample advances the time by its own interval (1 ms at least).
struct Sim
{
    ext::fg::DynamicMfg sel;
    uint64_t t = 1000;
    int factor = 2;
    int maxFactor = 4;
    double target = 224.0;
    bool probe = true;
    std::vector<Switch> log;

    // One sample.
    int Step(double ms)
    {
        t += (uint64_t) std::max(1.0, std::isfinite(ms) && ms < 1000.0 ? std::floor(ms) : 1.0);
        const int next = sel.Select(ms, target, t, factor, maxFactor, probe);
        if (next != factor)
            log.push_back({ t, factor, next, sel.Last().why });
        factor = next;
        return factor;
    }

    // Samples whose interval depends on the factor in force, for `durMs` of simulated time.
    void Run(uint64_t durMs, const std::function<double(int)>& msFor)
    {
        const uint64_t end = t + durMs;
        while (t < end)
            Step(msFor(factor));
    }
    void Run(uint64_t durMs, double ms)
    {
        Run(durMs, [ms](int) { return ms; });
    }

    // Switches (any kind) at or after `from`, optionally without trial switches.
    int Count(uint64_t from, bool withTrials = true) const
    {
        int n = 0;
        for (const auto& s : log)
            if (s.t >= from && (withTrials || (s.why != ext::fg::DynamicMfg::Why::Probe &&
                                               s.why != ext::fg::DynamicMfg::Why::ProbeRevert)))
                ++n;
        return n;
    }
    // Time of the first switch at or after `from` (0 = none).
    uint64_t First(uint64_t from) const
    {
        for (const auto& s : log)
            if (s.t >= from)
                return s.t;
        return 0;
    }
    int CountWhy(ext::fg::DynamicMfg::Why why) const
    {
        int n = 0;
        for (const auto& s : log)
            n += s.why == why;
        return n;
    }
};

// The closed-loop plant of the study (5.1): the real-frame interval at factor F (generated frames + 1) in ms.
//   ms(F) = max(base2x + 1.5 * (F - 2),  1000 * F / refresh,  2000 / limit)
// first term: cost of a generated frame; second: a refresh-bound output pins the presented rate at the refresh;
// third: the limiter counts generated frames too, so the real rate is capped at limit / 2 whatever the factor.
// refresh <= 0 / limit <= 0 drop the term.
struct Plant
{
    double base2x = 8.0, perStep = 1.5, refresh = 240.0, limit = 224.0;
    double Ms(int f) const
    {
        double ms = base2x + perStep * (f - 2);
        if (refresh > 0)
            ms = std::max(ms, 1000.0 * f / refresh);
        if (limit > 0)
            ms = std::max(ms, 2000.0 / limit);
        return ms;
    }
    double Presented(int f) const { return f * 1000.0 / Ms(f); }
};
