@{
    Class       = 'cpu'
    Note        = 'Window mode step 3 on fakes: a mode switch made with DLSS-G running is held back, frame generation goes off first, the call is replayed after it is confirmed off (never with it on before the hard limit)'
    Sources     = @('window_defer_flow.cpp')
    IncludeDirs = @('.', '{repo}/OptiScaler')
}
