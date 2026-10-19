@{
    Class       = 'cpu'
    Note        = 'Window mode (R3-L2): the lock wiring of the live module is pinned by a source check (each call out has its unlock scope before the call, each entry point takes and counts the lock)'
    Sources     = @('window_lock_wiring.cpp')
    IncludeDirs = @('.', '{repo}/OptiScaler')
    Args        = @('{repo}')
}
