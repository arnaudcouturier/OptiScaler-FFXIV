#pragma once
// Test stand-in for the product precompiled header. The recorder core and its
// seams build against only the Windows platform headers and the C++ standard
// library here.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// The product logs through spdlog; the core tests never initialize it, so a
// failure log is a no-op here (the error code is what the tests assert).
#ifndef LOG_WARN
#define LOG_WARN(msg, ...) ((void) 0)
#endif
