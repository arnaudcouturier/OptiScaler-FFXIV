@{
    Class       = 'cpu'
    Note        = 'NR runtime switch state machine (NrRuntimeSwitch.h) against a fake pass: step order, old-model failure forgotten, unavailable model and recovery, 20 rapid switches end in the last selection with no leaked feature'
    Sources     = @('nr_runtime_switch.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
}
