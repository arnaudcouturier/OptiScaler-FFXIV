@{
    Class       = 'cpu'
    Note        = 'FG recorder ring: steady-state taps allocate nothing (deque blocks fail this)'
    Sources     = @('fgrec_alloc.cpp')
    IncludeDirs = @('stubs', '.', '{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
