@{
    Class       = 'cpu'
    Note        = '[REL] The order of a release (ext/dx11bridge/ReleaseSequence.h): close the handle, wait for evaluates in flight, drain FG then upscale queue, classify; the table lock is never held across the wait or a drain; reopen on failure or exception; key off closes nothing and waits for nothing'
    Sources     = @('release_sequence_test.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('UNDEBUG')
}
