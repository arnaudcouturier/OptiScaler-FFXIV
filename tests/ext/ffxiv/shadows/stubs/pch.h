#pragma once
// Stand-in for the product pch.h in the contact shadow tests: the system headers the production sources expect and
// counting log macros (the injection test asserts how many lines a refusal path writes).
#include <windows.h>
#include <d3d11_1.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
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

// Used by the config sources that some tests link (ExtConfig.cpp).
inline std::string wstring_to_string(const std::wstring& value) { return std::string(value.begin(), value.end()); }
inline std::wstring string_to_wstring(const std::string& value) { return std::wstring(value.begin(), value.end()); }
