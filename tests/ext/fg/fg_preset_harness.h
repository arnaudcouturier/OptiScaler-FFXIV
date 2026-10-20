#pragma once
// Tiny CHECK harness shared by the DLSS-G preset tests (ext/fg/dlssg/Preset*.h).
#include <cstdio>

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
