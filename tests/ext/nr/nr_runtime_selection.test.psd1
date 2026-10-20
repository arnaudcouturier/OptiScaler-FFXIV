@{
    Class       = 'cpu'
    Note        = 'NR runtime selection (NrRuntimeSelection.h): immutable snapshot with revision, 250 ms coalescing of rapid requests, revision moves only when the model changes, rescan'
    Sources     = @('nr_runtime_selection.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
}
