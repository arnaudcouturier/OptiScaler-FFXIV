@{
    Class       = 'cpu'
    Note        = 'Live FG output switch runtime helpers: the status hand-off (fields first, state last), the one-bridge registry and its session refusal, the hook detach gate and its in-flight counter'
    Sources     = @('fg_switch_runtime.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
