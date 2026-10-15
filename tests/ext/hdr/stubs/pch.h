#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <vector>

#define LOG_INFO(...) ((void)0)
#define LOG_ERROR(...) ((void)0)

inline std::string wstring_to_string(const std::wstring& value)
{
    return std::string(value.begin(), value.end());
}

inline std::wstring string_to_wstring(const std::string& value)
{
    return std::wstring(value.begin(), value.end());
}
