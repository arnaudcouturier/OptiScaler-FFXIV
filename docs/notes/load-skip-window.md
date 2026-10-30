# The LoadLibrary skip window is per thread (fixed)

`State::DisableChecks(...)` / `EnableChecks(...)` / `SkipDllName()` (upstream code in `State.h`) open a window in which the
`LoadLibrary` hook runs no checks for the loads it makes: the plugin branches, the denylist and the catch-up for modules
already in memory are all skipped, and the load goes straight to the real loader. Four call sites use it, each around a call
that loads modules itself:

- `hooks/Dxgi_Hooks.cpp` (`hkCreateDXGIFactory`, `1`, `2`): `"dxgi"` around the original `CreateDXGIFactory*`.
- `fsr4/FSR4Upgrade.cpp`: every name around `UpdateFfxApiProviderEx` / `UpdateFfxApiProvider`, which load the AMD driver.
- `hooks/Streamline_Hooks.cpp` (`hookInterposer`): `"sl.interposer"`, because reading the DLL version loads modules.
- `proxies/Streamline_Proxy.h` (`LoadStreamline`, `Init`): `"sl."` around `sl.interposer.dll` and around `slInit`, which
  loads Streamline's own plugins from the local folder.

The state was four static members: a `std::map<UINT, std::string>` of open windows, the current owner id, an "enabled" flag
and a counter handing out the owner ids. Two things were wrong with it.

- **It was shared by every thread.** `DisableChecks` wrote the map while the load hook read it (`SkipDllName()`'s `contains`
  and `operator[]`) on whatever thread was making a load, and the id counter was incremented and read as two operations.
  That is undefined behaviour: a corrupted map, a crash, and in a stress run an infinite loop inside the tree.
- **One thread's window applied to every thread's loads.** A load made anywhere in the process while any window was open
  skipped the checks of the window's owner, so the module it loaded was neither tested against the denylist nor hooked, and
  the catch-up did not run. The window outlived its owner on the other threads, and two windows that ended out of order
  removed whichever entry happened to be current.

## The rule now

- **The window belongs to the thread that opened it.** The three members are `thread_local`, so a load made by another
  thread is checked, hooked and counted as usual, and the hook never touches state another thread writes. Every caller
  wraps a call that loads on its own thread, so no caller wanted the cross-thread effect.
- **A window ends with the owner id it was opened with.** `EnableChecks(owner)` erases that id (it ignored its parameter
  and erased the newest window instead). The open windows are still keyed by id, and the newest one still open decides the
  skipped name, so nesting (`"sl."` around `"sl.interposer"`) behaves as before, also when the outer window closes first.
- **No lock is involved.** `thread_local` needs none, so nothing is ever held across a load and the skip window adds no new
  edge to the loader-lock order that `docs/notes/streamline-loader-lock-hazard.md` describes: a private lock taken around
  the map would have been a lock that a loader callout could reach, which is what Microsoft's "Dynamic-Link Library Best
  Practices" page warns about.
- The owner id counter is a `std::atomic<UINT>`, so two threads can no longer receive the same id.

## What the tests check

- `tests/ext/fg/skip_window_threads.cpp` (cpu): the window of one thread is invisible to another; a window ends with its
  owner id, out of order too; three open windows, the newest still open decides the name; closing an id that opened nothing
  changes nothing; a window left open by a thread that exited disappears with it; ids handed out by eight threads at once
  are all different.
- `tests/ext/fg/skip_window_race.cpp` (cpu, built with the debug CRT by `run_skip_window_race.ps1`): six threads open and
  close two nested windows each while two threads that opened none check that nothing leaks into them, under a 90 s
  deadline. On the old code this run trips the debug heap's block check and does not finish.
- Both read the production code, sliced out of `State.h` by `prepare_skip_window_state.ps1`.
- Eleven deliberate breakages of the fix (each `thread_local` dropped, the old `EnableChecks` erase, a plain counter, a
  constant id, the flag left on or never set, the oldest window deciding the name) are all caught by these two tests.

## Not covered

- `_serveOriginal` / `_serveOwner` next to the window are still plain members and are written from `DllMain` at start-up.
  They are a separate flag (whether a hook serves the original function) and are not part of this fix.
- Whether a load started on a second thread from inside a `DllMain` of a module the window's owner loaded should also be
  skipped: it is checked now. That is the intended change, and no caller in the tree relies on the old behaviour.
- Nothing here was measured in a game: the defect is a race plus a wrong skip, and the tests run on this machine only.
