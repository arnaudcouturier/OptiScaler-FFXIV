@{
    Class       = 'cpu'
    Note        = 'Window mode: the live Env out-calls on fakes (frame generation request order, replay with a null target and the HRESULT passed back) and a source check that LiveEnv goes through them'
    Sources     = @('window_live_ops.cpp')
    IncludeDirs = @('.', '{repo}/OptiScaler')
    Args        = @('{repo}')
}
