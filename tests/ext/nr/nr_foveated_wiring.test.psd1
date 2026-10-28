@{
    Class       = 'cpu'
    Note        = 'Source-text pins: the NR pass chooses one geometry for every consumer, parks only what a change invalidates, allocates each scratch on its own check; both shaders branch on the foveated profile'
    Sources     = @('nr_foveated_wiring.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
    Std         = 'c++20'
}
