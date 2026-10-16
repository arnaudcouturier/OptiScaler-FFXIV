#pragma once
// Seam header of the ReShade built-in add-on (docs/SEAMS-catalog.md group `reshade`), included by ext/Seams.h.
// Deliberately free of ReShade and ImGui headers: upstream files only ever see this declaration.

#include <Windows.h>

namespace ext::reshade
{
// dllmain.cpp CheckWorkingMode, right after the `LoadReShade` block. Runs inside DllMain (loader lock): no file or
// folder I/O, no threads, no COM. `reshadeModule` is the module OptiScaler just loaded and may be null (LoadReShade
// off, or ReShade loaded by something else: the process is then searched for a module exporting ReShadeRegisterAddon).
// Registers OptiScaler's own module as a ReShade add-on (API 20) and logs ONE line: "registered (API 20)" or the
// reason it stays inert. Idempotent, never throws, never blocks.
void OnReshadeLoaded(HMODULE reshadeModule);
} // namespace ext::reshade
