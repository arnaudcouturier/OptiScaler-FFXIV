@{
    Class       = 'cpu'
    Note        = 'Dynamic MFG kill-switch bounds: the sample counts and the time span a bad status or count has to persist, and the signature window, each pinned on both sides with exact tick times'
    Sources     = @('dynamic_mfg_killswitch_bounds.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
}
