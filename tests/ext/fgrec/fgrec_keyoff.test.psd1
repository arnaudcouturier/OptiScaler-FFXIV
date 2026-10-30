@{
    Class       = 'cpu'
    Note        = 'FG recorder key off: taps cost one load with no thread, allocation or file, the off gate precedes any record build, and switching off after a session drains, ends and stops by itself (a tap that writes while off fails)'
    Sources     = @('fgrec_keyoff.cpp')
    IncludeDirs = @('stubs', '.', '{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
