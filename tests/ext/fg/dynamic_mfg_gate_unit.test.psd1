@{
    Class       = 'cpu'
    Note        = 'Dynamic MFG gating table (every gate false once), target and ceiling arithmetic, eDynamic -> eOn and Ada 4x ceiling guard'
    Sources     = @('dynamic_mfg_gate_unit.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
}
