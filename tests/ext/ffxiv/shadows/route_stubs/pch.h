#pragma once
// Stand-in for the product pch.h in the shadow mapping route test: the system headers the production sources expect and log
// macros that keep EVERY line (the test asserts which once-per-state lines were written, and in what order).
#include <windows.h>
#include <d3d11_1.h>
#include <wrl/client.h>

#include "ext/ExtFeatures.h" // the feature bits the product pch makes visible to every source

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <utility>
#include <vector>

inline std::vector<std::string> g_testLog;

template <class... A> inline void TestLog(std::format_string<A...> fmt, A&&... args)
{
    g_testLog.push_back(std::format(fmt, std::forward<A>(args)...));
}

#define LOG_DEBUG(...) ((void) 0)
#define LOG_INFO(...) TestLog(__VA_ARGS__)
#define LOG_WARN(...) TestLog(__VA_ARGS__)
#define LOG_ERROR(...) TestLog(__VA_ARGS__)

// Used by the config sources this test links (ExtConfig.cpp).
inline std::string wstring_to_string(const std::wstring& value) { return std::string(value.begin(), value.end()); }
inline std::wstring string_to_wstring(const std::string& value) { return std::wstring(value.begin(), value.end()); }
