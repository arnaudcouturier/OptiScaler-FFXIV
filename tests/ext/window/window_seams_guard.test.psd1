@{
    Class       = 'cpu'
    Note        = 'Window mode: with [Window] Mode = off every entry point is an inline guard on one atomic and reaches no body; probe and on reach them'
    Sources     = @('window_seams_guard.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
}
