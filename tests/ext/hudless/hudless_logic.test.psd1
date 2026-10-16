@{
    Class       = 'cpu'
    Note        = 'S10 engine logic: fallback reasons, state tracker hysteresis, coverage guard (trip/retry/unknown), slot ring reader fences (never overwrite a busy slot, never block), summary and state-change line formats'
    Sources     = @('hudless_logic.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Defines     = @('UNDEBUG', 'NOMINMAX')
}
