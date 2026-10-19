@{
    Class       = 'cpu'
    Note        = 'Dynamic MFG selector closed loop: plant with the refresh pin and the limiter cap (light/heavy scene, failed probe, grid, scene steps)'
    Sources     = @('dynamic_mfg_closed_loop.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
}
