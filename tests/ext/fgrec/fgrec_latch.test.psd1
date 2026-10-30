@{
    Class       = 'cpu'
    Note        = 'FG recorder failure latch: a failed start retries nothing over 1000 taps (handle growth bounded) and rearms only off/on'
    Sources     = @('fgrec_latch.cpp')
    IncludeDirs = @('stubs', '.', '{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
