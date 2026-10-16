// Stand-in for ReShade64.dll's add-on exports (S10 D0 tests only). Mode from the environment variable
// FAKE_RESHADE_MODE: "refuse" = ReShadeRegisterAddon returns false (older ReShade / add-ons disabled).
#include <Windows.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace
{
uint32_t g_api = 0;
void* g_module = nullptr;
int g_registerCalls = 0;
int g_events = 0;
int g_lastEvent = -1;
void* g_lastCallback = nullptr;
} // namespace

extern "C"
{
__declspec(dllexport) bool ReShadeRegisterAddon(void* module, uint32_t api_version)
{
    g_registerCalls++;
    g_module = module;
    g_api = api_version;
    char mode[32] = {};
    if (GetEnvironmentVariableA("FAKE_RESHADE_MODE", mode, sizeof(mode)) > 0 && std::strcmp(mode, "refuse") == 0)
        return false;
    return api_version <= 20;
}

__declspec(dllexport) void ReShadeUnregisterAddon(void*) {}

__declspec(dllexport) void ReShadeRegisterEvent(int ev, void* callback)
{
    g_events++;
    g_lastEvent = ev;
    g_lastCallback = callback;
}

__declspec(dllexport) void ReShadeUnregisterEvent(int, void*) { g_events--; }

// Test hook: what the fake saw.
__declspec(dllexport) void Fake_Info(uint32_t* api, void** module, int* registerCalls, int* events, int* lastEvent,
                                     void** lastCallback)
{
    *api = g_api;
    *module = g_module;
    *registerCalls = g_registerCalls;
    *events = g_events;
    *lastEvent = g_lastEvent;
    *lastCallback = g_lastCallback;
}
}
