@{
    Class       = 'cpu'
    Note        = 'The real picker glue (NrRuntimeCore.h) on temporary folders with a fake environment: catalog, switching, rescan, render thread never waits for a scan, hash worker (no logging, no re-read of unhashable or hashed files, shutdown), fail-open scan, session lock'
    Sources     = @('nr_runtime_core.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
