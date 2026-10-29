@{
    Class    = 'manual'
    Note     = 'Detours transaction race stress (D5), NOT runnable as a local test on this build: with the product OptiScaler.dll of the worktree staged next to an ini that turns the log on, the test process hangs (0 percent CPU, no exit after 40 s, even with 1 thread and 1 iteration) inside the first LoadLibraryW of a real sl.interposer.dll, after the log line StreamlineHooks::hookInterposer Hooking v2. The setup the source header documents (FGInput=upscaler, FGOutput=dlssg in the ini) was tried too: same hang. The recipe (Prepare script stages DLL, ini and a plugin copy; Requires and Args are set) is kept so it can become local once that hang is understood'
    Requires = @('OPTISCALER_SL_PLUGIN_DIR', '{repo}/x64/Release/a/OptiScaler.dll')
    Prepare  = 'prepare_streamline_detours_race_stress.ps1'
    Sources  = @('streamline_detours_race_stress.cpp')
    Args     = @('{out}/stage/OptiScaler.dll', '{out}/stage/plugins', '300', '3')
}
