@{
    Class       = 'cpu'
    Note        = 'ConcurrentInputSelection: 100000 concurrent updates per thread must never tear (D5, header from [UP] 4bf30433)'
    Sources     = @('low_latency_selection_unit.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('UNDEBUG')
}
