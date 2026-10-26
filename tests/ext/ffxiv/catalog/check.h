#pragma once
// Tiny assertion helper for the shader-catalog health tests (never disabled by NDEBUG).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _MSC_VER
#pragma warning(disable : 4127) // CHECK on a constant expression (pinned constants) is deliberate
#endif

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
            std::printf("CHECK_EQ FAILED %s:%d: %s == %s (%llu vs %llu)\n", __FILE__, __LINE__, #a, #b,                \
                        static_cast<unsigned long long>(va_), static_cast<unsigned long long>(vb_));                   \
            ++g_failures;                                                                                              \
        }                                                                                                              \
    } while (0)

// Substring test with the text printed on failure (log line checks).
#define CHECK_HAS(text, needle)                                                                                        \
    do                                                                                                                 \
    {                                                                                                                  \
        const std::string t_ = (text);                                                                                 \
        if (t_.find(needle) == std::string::npos)                                                                      \
        {                                                                                                              \
            std::printf("CHECK_HAS FAILED %s:%d: \"%s\" not in \"%s\"\n", __FILE__, __LINE__, needle, t_.c_str());     \
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
    std::printf("PASS: %s (all checks passed)\n", name);
    return 0;
}
