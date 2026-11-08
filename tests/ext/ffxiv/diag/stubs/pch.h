#pragma once
// Stand-in for the product pch.h when a test builds one of the ext sources on its own: the system headers the draw diagnostic's runtime
// expects, and log macros that record nothing (the rule under test writes no line).
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#define LOG_DEBUG(...) ((void) 0)
#define LOG_INFO(...) ((void) 0)
#define LOG_WARN(...) ((void) 0)
#define LOG_ERROR(...) ((void) 0)
