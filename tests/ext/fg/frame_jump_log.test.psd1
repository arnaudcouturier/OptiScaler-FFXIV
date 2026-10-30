@{
    Class       = 'cpu'
    Note        = 'the frame-count re-sync warning''s rate limit: first jump logged verbatim, one line per second with the count it swallowed, a backwards clock logs instead of hiding'
    Sources     = @('frame_jump_log.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Std         = 'c++20'
}
