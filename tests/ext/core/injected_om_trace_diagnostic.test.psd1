@{
    Class       = 'cpu'
    Note        = 'Checks disabled tracing, bounded rows, nesting, exception commit, and foreign-thread attribution.'
    Sources     = @('injected_om_trace_diagnostic.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Std         = 'c++latest'
}
