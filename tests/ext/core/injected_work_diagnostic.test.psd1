@{
    Class       = 'cpu'
    Note        = 'Opt-in injected GPU observer suppression is thread-local, nest-safe, inactive by default, and restores after exceptions'
    Sources     = @('injected_work_diagnostic.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
}
