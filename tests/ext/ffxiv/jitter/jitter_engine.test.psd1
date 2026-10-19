@{
    Class       = 'cpu'
    Note        = 'Jitter engine log behaviour against a fake environment: the status line and its counts, the budgets of the sample, status and episode lines, the report of episodes that were not logged'
    Sources     = @('jitter_engine.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
