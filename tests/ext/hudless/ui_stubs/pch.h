#pragma once
// Stand-in for the product pch.h in the standalone S10 GPU tests (package C). Besides the Windows/D3D12 headers the
// product pch brings in, it routes LOG_INFO into ui_test::info so the tests can check the exact first-success lines.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <vector>

namespace ui_test
{
inline std::mutex infoMutex;
inline std::vector<std::string> info;
inline void Info(std::string line)
{
    std::lock_guard lock(infoMutex);
    info.push_back(std::move(line));
}
inline int CountInfo(const std::string& prefix)
{
    std::lock_guard lock(infoMutex);
    int n = 0;
    for (const auto& line : info)
        n += line.rfind(prefix, 0) == 0;
    return n;
}
} // namespace ui_test

#define LOG_INFO(...) ui_test::Info(std::format(__VA_ARGS__))
#define LOG_WARN(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
#define LOG_DEBUG(...) ((void)0)
