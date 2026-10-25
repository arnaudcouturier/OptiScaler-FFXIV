#pragma once
// Stand-in for the product pch.h in the standalone ReShade add-on tests (see tests/ext/gpu/stubs): Windows, std::format
// and log macros are all the add-on sources need. ImGui must NOT be visible here (ReShadeRegister.cpp checks).
//
// The log macros are silent unless the test defines DEPTH_TEST_LOG: then every line is kept (formatted with std::format, so the
// format string and its arguments are checked) in depthtest_log::Lines() for the test to look at.
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstdint>
#include <format>
#include <string>

#ifdef DEPTH_TEST_LOG
#include <vector>
namespace depthtest_log
{
inline std::vector<std::string>& Lines()
{
    static std::vector<std::string> lines;
    return lines;
}
} // namespace depthtest_log
#define LOG_INFO(msg, ...) ::depthtest_log::Lines().push_back("I " __FUNCTION__ " " + std::format(msg, ##__VA_ARGS__))
#define LOG_WARN(msg, ...) ::depthtest_log::Lines().push_back("W " __FUNCTION__ " " + std::format(msg, ##__VA_ARGS__))
#define LOG_ERROR(msg, ...) ::depthtest_log::Lines().push_back("E " __FUNCTION__ " " + std::format(msg, ##__VA_ARGS__))
#define LOG_DEBUG(msg, ...) ::depthtest_log::Lines().push_back("D " __FUNCTION__ " " + std::format(msg, ##__VA_ARGS__))
#else
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
#define LOG_DEBUG(...) ((void)0)
#endif

// The one thing of State the add-on callbacks read (the product State.h is far too big to stand in for).
struct State
{
    bool isShuttingDown = false;
    static State& Instance()
    {
        static State state;
        return state;
    }
};
