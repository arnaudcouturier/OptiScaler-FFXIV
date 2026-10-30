#pragma once
// Stand-in for the product pch.h in the tick-probe tests: the system headers, a capturing log, and QueryPerformanceCounter redirected to a fake
// microsecond clock the test advances by hand (the probe's 10 s window can then close inside a test run).
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <utility>
#include <vector>

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

inline std::string wstring_to_string(const std::wstring& value)
{
    std::string out;
    out.reserve(value.size());
    for (wchar_t c : value)
        out.push_back(static_cast<char>(c));
    return out;
}
inline std::wstring string_to_wstring(const std::string& value)
{
    std::wstring out;
    out.reserve(value.size());
    for (char c : value)
        out.push_back(static_cast<wchar_t>(c));
    return out;
}

// The fake clock, defined by the test: a frequency of 1,000,000 makes one QPC unit one microsecond.
BOOL TestQueryPerformanceCounter(LARGE_INTEGER* value);
BOOL TestQueryPerformanceFrequency(LARGE_INTEGER* value);

#define QueryPerformanceCounter TestQueryPerformanceCounter
#define QueryPerformanceFrequency TestQueryPerformanceFrequency
