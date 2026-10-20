// Stand-in for nvngx_dlssnr.dll in nr_same_name_modules: built twice with a different PROBE_ID, both copies named alike.
#include <windows.h>
#ifndef PROBE_ID
#define PROBE_ID 0
#endif
extern "C" __declspec(dllexport) int dlssnr_probe_id() { return PROBE_ID; }
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
