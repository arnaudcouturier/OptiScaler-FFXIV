@{
    Class       = 'cpu'
    Note        = '[REL] Thread-id lines at create, evaluate and release (ext/upscaler/NgxThreadTraceCore.h): pinned log format, evaluate logging is rate-limited (first call, a thread change, then at most one line per interval with the count), per-key state is bounded'
    Sources     = @('ngx_thread_trace_test.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('UNDEBUG')
}
