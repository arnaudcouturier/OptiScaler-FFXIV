@{
    Class       = 'cpu'
    Note        = 'Window mode: the pure border rule (clear the edge bits of a frame-less window, leave a real windowed mode alone)'
    Sources     = @('window_clear_edge.cpp')
    IncludeDirs = @('.', '{repo}/OptiScaler')
}
