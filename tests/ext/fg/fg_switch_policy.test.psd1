@{
    Class       = 'cpu'
    Note        = 'Live FG output switch preflight: every verdict and refusal class, per-target memory, cooldown, session budget, the resize windows at their bounds'
    Sources     = @('fg_switch_policy.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
