#pragma once
// Stand-in for the product pch.h in fg_preset_drs: Windows, the real NVAPI types, and a silent LOG_INFO.
#include <windows.h>

#include <nvapi.h>

#include <atomic>
#include <cstdint>
#include <string>

#define LOG_INFO(...) ((void) 0)
