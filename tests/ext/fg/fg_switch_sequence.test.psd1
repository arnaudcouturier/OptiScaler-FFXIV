@{
    Class       = 'cpu'
    Note        = 'Live FG output switch sequence: step order, preflight refusals, rollback to plain, limp window, budget, cooldown, probation rollback, device loss'
    Sources     = @('fg_switch_sequence.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
