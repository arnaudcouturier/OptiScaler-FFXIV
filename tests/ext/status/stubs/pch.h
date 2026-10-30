#pragma once
// Test stand-in for the product precompiled header. The status writer tests build
// production files that only need the Windows platform headers and the logging
// macros from it; the configuration object comes from the Config.h stand-in below.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "Config.h"

#define LOG_INFO(...) ((void) 0)
#define LOG_WARN(...) ((void) 0)
#define LOG_ERROR(...) ((void) 0)
