@{
    Class       = 'cpu'
    Note        = 'SuccessfulFrames history rule of the FG reset decisions (C5/B5); stands in for dlssg_delayed_guides, see PORT-LOG S5'
    Sources     = @('temporal_continuity_unit.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('UNDEBUG')
}
