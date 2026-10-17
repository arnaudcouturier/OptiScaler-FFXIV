#pragma once
// Stand-in for the product pch.h in the character preview tests: the system headers the production sources expect and
// counting log macros (the tests assert how many lines a failure path writes).
#include <windows.h>
#include <d3d11_1.h>
#include <wrl/client.h>

#include <cstdint>
#include <format>
#include <string>
#include <utility>

inline int g_testLogLines = 0;
inline std::string g_testLastLog;

template <class... A> inline void TestLog(std::format_string<A...> fmt, A&&... args)
{
    ++g_testLogLines;
    g_testLastLog = std::format(fmt, std::forward<A>(args)...);
}

#define LOG_DEBUG(...) ((void) 0)
#define LOG_INFO(...) TestLog(__VA_ARGS__)
#define LOG_WARN(...) TestLog(__VA_ARGS__)
#define LOG_ERROR(...) TestLog(__VA_ARGS__)
