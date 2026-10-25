// Stand-in for ReShade64.dll's add-on exports (S10 D0 tests only). Mode from the environment variable
// FAKE_RESHADE_MODE: "refuse" = ReShadeRegisterAddon returns false (older ReShade / add-ons disabled); "crash" = it faults (a ReShade
// that is half unloaded). FAKE_RESHADE_CRASH_EVENT=<n>: ReShadeRegisterEvent faults for the event with that number.
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
// every registration, in order (the late-registration test checks WHICH events were subscribed, and how often)
constexpr int kMaxRecorded = 64;
int g_recordedEvent[kMaxRecorded] = {};
void* g_recordedCallback[kMaxRecorded] = {};
int g_recorded = 0;
int g_eventCalls = 0; // every ReShadeRegisterEvent call, also the ones that faulted
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
    if (GetEnvironmentVariableA("FAKE_RESHADE_MODE", mode, sizeof(mode)) > 0 && std::strcmp(mode, "crash") == 0)
    {
        volatile int* null = nullptr;
        *null = 1;
    }
    return api_version <= 20;
}

__declspec(dllexport) void ReShadeUnregisterAddon(void*) {}

__declspec(dllexport) void ReShadeRegisterEvent(int ev, void* callback)
{
    ++g_eventCalls;
    char crash[16] = {};
    if (GetEnvironmentVariableA("FAKE_RESHADE_CRASH_EVENT", crash, sizeof(crash)) > 0 && std::atoi(crash) == ev)
    {
        volatile int* null = nullptr;
        *null = 1;
    }
    g_events++;
    g_lastEvent = ev;
    g_lastCallback = callback;
    if (g_recorded < kMaxRecorded)
    {
        g_recordedEvent[g_recorded] = ev;
        g_recordedCallback[g_recorded] = callback;
        ++g_recorded;
    }
}

__declspec(dllexport) void ReShadeUnregisterEvent(int, void*) { g_events--; }

// Test hook: how many times ReShadeRegisterEvent was called, the faulting calls included.
__declspec(dllexport) int Fake_EventCalls() { return g_eventCalls; }

// Test hook: every ReShadeRegisterEvent call so far (up to `capacity` entries are copied); returns how many calls there were.
__declspec(dllexport) int Fake_Events(int* events, void** callbacks, int capacity)
{
    for (int i = 0; i < g_recorded && i < capacity; ++i)
    {
        events[i] = g_recordedEvent[i];
        callbacks[i] = g_recordedCallback[i];
    }
    return g_recorded;
}

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
