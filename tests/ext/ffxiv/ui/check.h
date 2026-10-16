#pragma once
// Tiny assertion helper for the S10-A tests (never disabled by NDEBUG).
#include <cstdio>
#include <cstdlib>

inline int g_failures = 0;

#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::printf("CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);                                        \
            ++g_failures;                                                                                              \
        }                                                                                                              \
    } while (0)

#define CHECK_EQ(a, b)                                                                                                 \
    do                                                                                                                 \
    {                                                                                                                  \
        const auto va_ = (a);                                                                                          \
        const auto vb_ = (b);                                                                                          \
        if (!(va_ == vb_))                                                                                             \
        {                                                                                                              \
            std::printf("CHECK_EQ FAILED %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b,               \
                        static_cast<long long>(va_), static_cast<long long>(vb_));                                    \
            ++g_failures;                                                                                              \
        }                                                                                                              \
    } while (0)

inline int Finish(const char* name)
{
    if (g_failures)
    {
        std::printf("%s: %d check(s) failed\n", name, g_failures);
        return 1;
    }
    std::printf("%s: all checks passed\n", name);
    return 0;
}
