@{
    Class       = 'cpu'
    Note        = 'Jitter source fix, pure core: phase count (8/18/24/32/72), native strength, Halton normalisation to the full lattice, projected offset from NDC, ring pairing with staleness and mismatch, verification watchdog, decision table, shared state, log text'
    Sources     = @('jitter_source_core.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
