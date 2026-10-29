@{
    Class       = 'cpu'
    Note        = 'Wiring pins of the frame-pacing GPU stage timers: key binding, ini fragment, manifest, seam calls with the failed-conversion cancel, fence-gated slot reports, live-pass guard, frame-scoped effects spans, menu-thread excl row, DONOTFLUSH-only reads, one warning per timer'
    Sources     = @('gpu_timer_wiring.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('UNDEBUG')
}
