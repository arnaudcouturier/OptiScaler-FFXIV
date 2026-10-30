# Streamline plugin loads: the loader-lock deadlock (fixed)

A lock-order inversion in OptiScaler's `LoadLibrary` hook (`hooks/LibraryLoad_Hooks.cpp`, upstream code) could stop the
process at 0 % CPU while Streamline's plugins loaded. It was found on 2026-10-29 while making the Streamline Detours stress
test runnable and never seen in a game. The fix is in the same file and touches nothing else. This note keeps the two
orders, so an old report can still be read, and the rule the code follows now.

## The two orders

The Windows loader lock (held for the whole of a DLL load, including every `DllMain` it runs) and the six Streamline plugin
hook mutexes (`StreamlineHooks::mutexHook*`) were taken in opposite orders:

- **mutex, then loader lock.** Loading `sl.dlss_g.dll` took `mutexHookDlssg` and `mutexHookLocalDlssg` and then loaded the
  DLL. The other four plugin branches did the same with their own mutex (`sl.dlss`, `sl.reflex`, `sl.pcl`, `sl.common`).
- **loader lock, then mutex.** Every hooked load that matched no special name ended in `CheckModulesInMemory()`, which took
  the same mutexes and waited for them. When that load was nested inside another load (a `DllMain`, a thread attach, a TLS
  callback), the thread already held the loader lock.

One thread in a plugin branch holding its mutex while a second thread inside a loader callout waited for that mutex is a
cycle: the process stops, no crash and no log line. In FFXIV the exposed branch was `sl.dlss_g`, once per start, a window of
tens of milliseconds; the stress test reproduced the hang every time (the thread loading `sl.interposer.dll` waited in
`CheckModulesInMemory()` through a nested `dbghelp.dll` load while the thread loading `sl.dlss_g.dll` waited for the loader
lock).

## The rule now

- **A plugin hook mutex is never held across a DLL load.** The five plugin branches load the module first and take their
  mutex only around the hook call. Two threads loading the same plugin now overlap in the loader instead of queueing, which
  is the loader's own job; `hookPlugin` returns early for a module that is already hooked.
- **The catch-up never waits for one.** `CheckModulesInMemory()` takes each plugin mutex with `try_to_lock` and only does its
  work when it owns it: a busy mutex means another thread is loading or hooking that plugin right now, and that thread hooks
  what it finds. A skipped plugin is normally hooked by the thread that holds its mutex, or when the next load that matches no special name
  retries; this is not guaranteed in every corner case (another copy of the plugin, a module freed in between, the DLSS-G
  site that needs both mutexes). Start-up already hooks every plugin in memory.

Both rules follow from Microsoft's "Dynamic-Link Library Best Practices" page, which puts the loader lock at the top of the
locking hierarchy, forbids waiting for an object owned by code that is waiting for the loader lock, and says that a function
which may take the loader lock such as `GetModuleFileName` is called before a private lock is taken.

## What the tests check

- `tests/ext/fg/plugin_mutex_loader_lock_probe.cpp` (local): an empty `sl.dlss_g.dll` is loaded on one thread while another
  thread sits inside a `DllMain` that holds the loader lock and then makes one ordinary `LoadLibrary` call. On the old order
  the two threads wait for each other; now both finish. It runs under an out-of-process watchdog, so a hang is a failure with
  every thread stack and the owner of the loader lock printed.
- `tests/ext/fg/streamline_detours_race_stress.cpp` (local): the real Streamline plugins from
  `OPTISCALER_SL_PLUGIN_DIR` loaded concurrently through the product DLL, under the same watchdog; it also fails on a logged
  `10DD` Detours transaction failure. It hung every time on the old order.
- `tests/ext/fg/library_load_lock_order_pin.cpp` (cpu): reads the hook as text and fails if a plugin mutex is taken before the
  load in any of the five branches, or if the catch-up takes a plugin mutex without `try_to_lock` and an `owns_lock()` check.

## Not covered, by design

- `hookPlugin` and `isSlotAlive` still call `GetModuleFileNameW` while a plugin mutex is held. Microsoft says that function
  can take the loader lock, so a strict reading wants the path resolved before the mutex is taken. Neither test can reach
  that case: the empty plugin has no `slGetPluginFunction`, the hook returns before it reads a module path, and the loader
  lock is free again by the time the hook runs.
- A plugin branch reached from inside a loader callout still waits for its mutex. Streamline loads its plugins from its own
  thread, and its documentation forbids calling `slInit` from `DllMain` for the same reason.

## Freezes in old reports

A report from before this fix that shows the game frozen during start-up, around the Streamline plugin log lines
(`Loaded plugin 'sl.dlss_g'`), with 0 % CPU and no crash dump, may be this deadlock. It is gone once the builds are from a
revision with the two rules above.
