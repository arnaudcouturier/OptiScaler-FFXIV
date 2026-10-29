# Streamline plugin loads: a latent loader-lock deadlock (known, not fixed)

Found on 2026-10-29 while making the Streamline Detours stress test (`tests/ext/fg/streamline_detours_race_stress.cpp`) runnable. Never seen in a game. Recorded here so a launch freeze can be recognised if one is ever reported.

**What can happen.** Two locks are taken in opposite orders by OptiScaler's `LoadLibrary` hook (`hooks/LibraryLoad_Hooks.cpp`, upstream code):
- Loading `sl.dlss_g.dll` (the branch for the DLSS-G plugin, about line 188) takes `StreamlineHooks::mutexHookDlssg` (and the local DLSS-G mutex) and then loads the DLL, which needs the Windows loader lock.
- Every other hooked load that matches no special name ends in `CheckModulesInMemory()` (about line 1057), which takes `mutexHookDlssg`. When that load is nested inside another load (a DLL's import or `DllMain` loading a DLL), the thread already holds the loader lock.

If one thread is inside the `sl.dlss_g.dll` branch while another thread performs a nested load at the same moment, each waits for the lock the other holds: the process stops at 0 % CPU, no crash and no log line.

**How it was shown.** The stress test starts one thread per Streamline plugin. The thread loading `sl.interposer.dll` held the loader lock while its import `dbghelp.dll` was loaded for the first time, re-entered the hook and waited for `mutexHookDlssg` in `CheckModulesInMemory`; the thread loading `sl.dlss_g.dll` held that mutex and waited for the loader lock. Stacks were taken from outside the process. Loading `dbghelp.dll` first, loading `sl.interposer.dll` first, or loading each plugin alone all pass.

**Why it is not fixed.**
- In FFXIV, Streamline loads its plugins one after another on one thread. The window is the few milliseconds of the `sl.dlss_g.dll` load, and only a DLL load on another thread at that moment (an injected tool, an overlay, OptiScaler's own start-up threads) could close it.
- No freeze at launch has been seen or reported.
- A fix changes upstream code that every Streamline start goes through.

**What a player would see:** the game freezing during start-up (before the first frame, around the Streamline log lines `Loaded plugin 'sl.dlss_g'`), with no crash dump. **What a fix would be:** not holding the plugin mutex across the DLL load, or making `CheckModulesInMemory` skip its work with a `try_lock` instead of waiting. The stress test, unchanged, hangs every time and would be the failing test for that fix.
