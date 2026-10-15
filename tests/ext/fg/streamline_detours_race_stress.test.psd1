@{
    Class   = 'manual'
    Note    = 'Detours transaction race stress (D5): needs a built OptiScaler.dll and real sl.*.dll plugins, args: <OptiScaler DLL> <Streamline plugin dir> [iters] [threads]'
    Sources = @('streamline_detours_race_stress.cpp')
}
