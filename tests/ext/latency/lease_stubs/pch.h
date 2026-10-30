#pragma once
// Stand-in for the product pch.h in the tick-lease tests: the system headers the product source expects, a counting log, and the Win32 calls the
// lease makes redirected to the harness. The product source is compiled with this header in front of it, so every redirect is declared here before the
// product uses it (a definition of a __declspec(dllimport) function is not allowed, which is why the names are macro-redirected instead).
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

// The harness hooks, defined by the test.
BOOL TestVirtualQuery(const void* address, MEMORY_BASIC_INFORMATION* info, SIZE_T size);
BOOL TestVirtualProtect(void* address, SIZE_T size, DWORD protect, PDWORD previous);
HMODULE TestGetModuleHandleW(LPCWSTR name);
BOOL TestGetModuleHandleExW(DWORD flags, LPCWSTR name, HMODULE* module);
void* TestInterlockedCompareExchangePointer(void* volatile* destination, void* exchange, void* comparand);

#define VirtualQuery TestVirtualQuery
#define VirtualProtect TestVirtualProtect
#define GetModuleHandleW TestGetModuleHandleW
#define GetModuleHandleExW TestGetModuleHandleExW
#undef InterlockedCompareExchangePointer
#define InterlockedCompareExchangePointer TestInterlockedCompareExchangePointer
