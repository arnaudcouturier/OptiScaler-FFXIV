@{
    Class       = 'cpu'
    Note        = 'FG recorder process-exit shutdown: a dead writer is taken over synchronously, every queued record plus session.end lands in the file (a missing end marker or a lost record fails)'
    Sources     = @('fgrec_shutdown.cpp')
    IncludeDirs = @('stubs', '.', '{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
