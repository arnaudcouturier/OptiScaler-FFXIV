#pragma once
// Stand-in for the product pch.h in the standalone ReShade add-on tests (see tests/ext/gpu/stubs): Windows, std::format
// and silent log macros are all the add-on sources need. ImGui must NOT be visible here (ReShadeRegister.cpp checks).
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstdint>
#include <format>
#include <string>

#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
#define LOG_DEBUG(...) ((void)0)
