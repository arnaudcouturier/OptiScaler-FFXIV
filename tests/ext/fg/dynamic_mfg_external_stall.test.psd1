@{
    Class       = 'cpu'
    Note        = 'AutoMFG expected-stall seam (NoteExternalStall): the next two intervals are ignored by the selector, the watch and the stall average; a pending stall is dropped while the gate is closed'
    Sources     = @('dynamic_mfg_external_stall.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
}
