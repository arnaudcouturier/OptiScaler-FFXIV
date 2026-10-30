@{
    Class       = 'cpu'
    Note        = 'FG recorder teardown taps: deactivating fires only when the feature is active with a cast label, one rebuild is one record (a global-state label or a doubled rebuild fails)'
    Sources     = @('fgrec_teardown.cpp')
    IncludeDirs = @('stubs', '.', '{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
