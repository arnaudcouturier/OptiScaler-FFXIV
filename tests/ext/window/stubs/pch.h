#pragma once
// Stand-in for the product pch.h in the window-mode tests that compile a module source (WindowWin32.cpp): Windows and the
// std headers the module sources use.
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstdint>
#include <format>
#include <string>
