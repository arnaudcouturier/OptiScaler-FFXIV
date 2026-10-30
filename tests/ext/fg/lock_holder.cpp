// Test DLL for plugin_mutex_loader_lock_probe: a DllMain already runs inside a DLL load, so it holds the Windows loader
// lock for as long as it takes. This one signals a named event (the name is in an environment variable, so two runs never
// share one), waits long enough for the probe's other thread to reach the plugin branch of the load hook, and then makes
// one ordinary LoadLibrary call. That nested call re-enters the hook on a thread that owns the loader lock.
#include <windows.h>

namespace
{
constexpr wchar_t kEventVariable[] = L"OPTISCALER_LOADER_LOCK_PROBE_EVENT";
constexpr DWORD kHoldMilliseconds = 400;
} // namespace

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;

    wchar_t eventName[256] = {};
    const DWORD length = GetEnvironmentVariableW(kEventVariable, eventName, (DWORD) (sizeof(eventName) / sizeof(eventName[0])));

    if (length > 0 && length < (DWORD) (sizeof(eventName) / sizeof(eventName[0])))
    {
        HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName);
        if (ready != nullptr)
        {
            SetEvent(ready);
            CloseHandle(ready);
        }
    }

    ::Sleep(kHoldMilliseconds);

    // advapi32.dll is already resident and matches no special branch of the load hook, so this call ends in the hook's
    // catch-up for modules already in memory, on a thread that holds the loader lock.
    LoadLibraryW(L"advapi32.dll");

    return TRUE;
}
