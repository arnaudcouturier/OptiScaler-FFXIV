@{
    Class       = 'cpu'
    Note        = 'Window mode step 1 on fakes: mode off is silent, probe logs one Window: line per resize and per set_fullscreen_state, never prevents'
    Sources     = @('window_probe_flow.cpp')
    IncludeDirs = @('.', '{repo}/OptiScaler')
}
