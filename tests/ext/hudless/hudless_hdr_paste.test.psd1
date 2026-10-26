@{
    Class       = 'cpu'
    Note        = '[HudLess] HdrPaste: the producer step of an HDR-output present. off keeps the old order (the HDR stage only on a probing present: the first image is never built: inferred from reading the code, never observed in a game), on ensures the stage before the producer and latches a failed creation until the HDR output state or the setting changes; fake engine cycle for both, SDR untouched, and the engine call site pinned by text'
    Sources     = @('hudless_hdr_paste.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('UNDEBUG', 'NOMINMAX')
    Std         = 'c++20'
}
