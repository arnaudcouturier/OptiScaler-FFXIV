@{
    Class       = 'cpu'
    Note        = 'Dynamic MFG controller: gating log lines, kill-switch outcomes, activation/gap restarts, state kept across them, self-test result line'
    Sources     = @('dynamic_mfg_control_unit.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
}
