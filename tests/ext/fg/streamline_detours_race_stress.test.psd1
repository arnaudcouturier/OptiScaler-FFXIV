@{
    Class    = 'local'
    Note     = 'Detours transaction race stress: real sl.*.dll plugins loaded concurrently through the product OptiScaler.dll of this worktree; FAIL on a logged 10DD hook failure or on a hang (an out-of-process launcher prints every thread stack and the owner of the loader lock). It hangs when a plugin hook mutex is held across the load and the catch-up waits for it: the thread loading sl.interposer.dll holds the loader lock and, through a nested dbghelp.dll load, waits in CheckModulesInMemory for the DLSS-G hook mutex, while the thread loading sl.dlss_g.dll holds that mutex and waits for the loader lock. Against a build without the Detours transaction mutex it still passed in 28 runs (the 10DD collision is rare)'
    Requires = @('OPTISCALER_SL_PLUGIN_DIR', '{repo}/x64/Release/a/OptiScaler.dll')
    Runner   = 'run_streamline_detours_race_stress.ps1'
    Sources  = @('streamline_detours_race_stress.cpp', 'hang_launcher.cpp')
}
