@{
    Class    = 'manual'
    Note     = 'Detours transaction race stress (D5), kept manual as a reproducer of a loader-lock deadlock (docs/notes/streamline-loader-lock-hazard.md): with the product OptiScaler.dll staged next to an ini that turns the log on and one thread per Streamline plugin loading at the same time, it hangs at 0 percent CPU. The thread loading sl.interposer.dll holds the loader lock and, through a nested dbghelp.dll load, waits in CheckModulesInMemory for the DLSS-G hook mutex; the thread loading sl.dlss_g.dll holds that mutex and waits for the loader lock. The hang is the expected result until that lock order changes. It does not detect the original Detours race: with the transaction mutex removed it still never failed'
    Requires = @('OPTISCALER_SL_PLUGIN_DIR', '{repo}/x64/Release/a/OptiScaler.dll')
    Prepare  = 'prepare_streamline_detours_race_stress.ps1'
    Sources  = @('streamline_detours_race_stress.cpp')
    Args     = @('{out}/stage/OptiScaler.dll', '{out}/stage/plugins', '300', '3')
}
