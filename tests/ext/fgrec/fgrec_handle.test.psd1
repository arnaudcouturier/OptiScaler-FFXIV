@{
    Class       = 'cpu'
    Note        = 'FG recorder session file handle: after a 16 MiB rotation the published handle is the live one, the final file ends with session.end and no closed handle is ever written, flushed, seeked or closed (a stale handle fails here); a kill inside a rotation leaves no handle for the exit takeover to act on; the exit takeover cuts a partial line, writes the queued records, appends session.end and allocates nothing (it runs inside DllMain); the stand-in file layer serializes its state for the test thread (a read waits for the lock, and every entry point takes it)'
    Sources     = @('fgrec_handle.cpp')
    IncludeDirs = @('stubs', '.', '{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
