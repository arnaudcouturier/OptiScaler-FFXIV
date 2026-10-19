@{
    Class       = 'cpu'
    Note        = 'Window mode: which SetFullscreenState calls are deferred (real change, DLSS-G, frame generation active, one thread) and which pass'
    Sources     = @('window_defer_decision.cpp')
    IncludeDirs = @('.', '{repo}/OptiScaler')
}
