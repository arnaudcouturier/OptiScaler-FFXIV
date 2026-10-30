@{
    Class       = 'cpu'
    Note        = 'The LoadLibrary skip window is per thread: a window opened by one thread is invisible to another, a window ends with the owner id that opened it (also when the outer one closes first), an unknown owner id changes nothing, and owner ids handed out on different threads are unique'
    Sources     = @('skip_window_threads.cpp')
    IncludeDirs = @('{out}')
    Prepare     = 'prepare_skip_window_state.ps1'
    Defines     = @('NOMINMAX')
}
