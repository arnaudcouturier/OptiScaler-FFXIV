#pragma once

#include "../stubs/pch.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#define NOMINMAX
#define LOG_INFO(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
