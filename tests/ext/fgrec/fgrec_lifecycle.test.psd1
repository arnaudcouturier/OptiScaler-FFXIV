@{
    Class       = 'cpu'
    Note        = 'FG recorder session lifecycle: session.begin opens and session.end closes a clean stop, detach only signals the worker and returns with records queued, and shutdown without start is a no-op (a missing end marker or a waiting shutdown fails)'
    Sources     = @('fgrec_lifecycle.cpp')
    IncludeDirs = @('stubs', '.', '{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
