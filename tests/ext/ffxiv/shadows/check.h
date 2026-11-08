#pragma once
// The shadows tests' shared check macros (the relight and engine test trees' check.h shape): a home for the count
// macros so each test file only carries its own expectations.
#include <cstdio>

inline int g_failures = 0;
#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            ++g_failures;                                                                                              \
            std::printf("    FAIL line %d: %s\n", __LINE__, #cond);                                                    \
        }                                                                                                              \
    } while (0)
#define CHECK_EQ(got, expected)                                                                                        \
    do                                                                                                                 \
    {                                                                                                                  \
        const auto g_ = (got);                                                                                         \
        const auto e_ = (expected);                                                                                    \
        if (!(g_ == e_))                                                                                               \
        {                                                                                                              \
            ++g_failures;                                                                                              \
            std::printf("    FAIL line %d: %s == %s (got %g, expected %g)\n", __LINE__, #got, #expected, double(g_),   \
                        double(e_));                                                                                   \
        }                                                                                                              \
    } while (0)
#define CHECK_LT(got, limit)                                                                                           \
    do                                                                                                                 \
    {                                                                                                                  \
        const auto g_ = (got);                                                                                         \
        const auto l_ = (limit);                                                                                       \
        if (!(g_ < l_))                                                                                                \
        {                                                                                                              \
            ++g_failures;                                                                                              \
            std::printf("    FAIL line %d: %s < %s (got %g)\n", __LINE__, #got, #limit, double(g_));                     \
        }                                                                                                              \
    } while (0)
#define CHECK_GT(got, limit)                                                                                           \
    do                                                                                                                 \
    {                                                                                                                  \
        const auto g_ = (got);                                                                                         \
        const auto l_ = (limit);                                                                                       \
        if (!(g_ > l_))                                                                                                \
        {                                                                                                              \
            ++g_failures;                                                                                              \
            std::printf("    FAIL line %d: %s > %s (got %g)\n", __LINE__, #got, #limit, double(g_));                    \
        }                                                                                                              \
    } while (0)
#define CHECK_LE(got, limit)                                                                                           \
    do                                                                                                                 \
    {                                                                                                                  \
        const auto g_ = (got);                                                                                         \
        const auto l_ = (limit);                                                                                       \
        if (!(g_ <= l_))                                                                                               \
        {                                                                                                              \
            ++g_failures;                                                                                              \
            std::printf("    FAIL line %d: %s <= %s (got %g)\n", __LINE__, #got, #limit, double(g_));                   \
        }                                                                                                              \
    } while (0)
#define CHECK_GE(got, limit)                                                                                           \
    do                                                                                                                 \
    {                                                                                                                  \
        const auto g_ = (got);                                                                                         \
        const auto l_ = (limit);                                                                                       \
        if (!(g_ >= l_))                                                                                               \
        {                                                                                                              \
            ++g_failures;                                                                                              \
            std::printf("    FAIL line %d: %s >= %s (got %g)\n", __LINE__, #got, #limit, double(g_));                   \
        }                                                                                                              \
    } while (0)
#define CHECK_STARTS(text, prefix)                                                                                     \
    do                                                                                                                 \
    {                                                                                                                 \
        if ((text).find(prefix) != 0)                                                                                  \
        {                                                                                                              \
            ++g_failures;                                                                                              \
            std::printf("    FAIL line %d: %s does not start with %s (got %.80s.....)\n", __LINE__, #text, #prefix,     \
                        (text).c_str());                                                                               \
        }                                                                                                              \
    } while (0)
#define CHECK_CONTAINS(text, needle)                                                                                   \
    do                                                                                                                 \
    {                                                                                                                  \
        if ((text).find(needle) == std::string::npos)                                                                  \
        {                                                                                                              \
            ++g_failures;                                                                                              \
            std::printf("    FAIL line %d: %s does not contain %s (got %.120s.....)\n", __LINE__, #text, #needle,       \
                        (text).c_str());                                                                               \
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
#define CHECK_NE(got, other)                                                                                          \
    do                                                                                                                 \
    {                                                                                                                  \
        const auto g_ = (got);                                                                                         \
        const auto o_ = (other);                                                                                       \
        if (!(g_ != o_))                                                                                               \
        {                                                                                                              \
            ++g_failures;                                                                                              \
            std::printf("    FAIL line %d: %s != %s (got %g)\n", __LINE__, #got, #other, double(g_));                    \
        }                                                                                                              \
    } while (0)
