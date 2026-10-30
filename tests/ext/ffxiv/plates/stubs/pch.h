#pragma once
// Stand-in for the product pch.h in the plate game-side test: the Windows and D3D headers the module includes, the
// standard library it uses, and LOG_* routed into a recording sink.
#include <windows.h>

#include <d3d11.h>
#include <d3d11_1.h>

#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <format>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace plate_game_test_pch
{
inline std::mutex logMutex;
inline std::vector<std::string> logLines;

inline void Log(std::string line)
{
    std::lock_guard lock(logMutex);
    logLines.push_back(std::move(line));
}

inline void ClearLog()
{
    std::lock_guard lock(logMutex);
    logLines.clear();
}
} // namespace plate_game_test_pch

#define LOG_INFO(...) plate_game_test_pch::Log(std::format(__VA_ARGS__))
#define LOG_WARN(...) LOG_INFO(__VA_ARGS__)
#define LOG_ERROR(...) LOG_INFO(__VA_ARGS__)
#define LOG_DEBUG(...) ((void)0)
