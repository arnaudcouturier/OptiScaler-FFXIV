@{
    Class       = 'cpu'
    Note        = 'Live FG output switch log lines: each step of the verification plan with its exact fields and tokens, and the failure-to-step mapping'
    Sources     = @('fg_switch_log.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
