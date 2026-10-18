@{
    Class       = 'cpu'
    Note        = 'Dynamic MFG selector (ext/fg/DynamicMfg.h), open loop: hold, dwell, spike clip, flap guard, probe-down, regret guard'
    Sources     = @('dynamic_mfg_unit.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
}
