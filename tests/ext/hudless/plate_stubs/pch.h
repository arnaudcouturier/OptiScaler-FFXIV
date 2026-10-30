#pragma once
// Stand-in for the product pch.h in the native nameplate engine test: the Windows and D3D headers the module includes,
// the standard library it uses, and LOG_* routed into a recording sink. Every log line also asserts that no leaf lock is
// held: logging is not one of the calls the plate engine's leaf mutex may contain.
#include <windows.h>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <vector>

#include "ext/hudless/PlateSync.h"

namespace plate_test
{
inline std::mutex logMutex;
inline std::vector<std::string> logLines;

inline void Log(std::string line)
{
    std::lock_guard lock(logMutex);
    logLines.push_back(std::move(line));
}

inline int CountLog(const std::string& prefix)
{
    std::lock_guard lock(logMutex);
    int n = 0;
    for (const auto& line : logLines)
        n += line.rfind(prefix, 0) == 0;
    return n;
}

inline void ClearLog()
{
    std::lock_guard lock(logMutex);
    logLines.clear();
}
} // namespace plate_test

#define LOG_INFO(...)                                                                                                  \
    do                                                                                                                 \
    {                                                                                                                  \
        ext::hudless::plates::sync::AssertNoLocksHeld("a log line was written under the leaf mutex");                    \
        plate_test::Log(std::format(__VA_ARGS__));                                                                      \
    } while (0)
#define LOG_WARN(...) LOG_INFO(__VA_ARGS__)
#define LOG_ERROR(...) LOG_INFO(__VA_ARGS__)
#define LOG_DEBUG(...) ((void)0)
