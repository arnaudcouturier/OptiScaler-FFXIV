@{
    Class       = 'cpu'
    Note        = 'Pure rules of the frame-pacing GPU stage timers: 4-of-16 sampling, 10 s window stats with p95 edges, per-slot pending with fence-gated recycle and first-writer frames, frame-scoped effects begins, excl-NR clamp, log line text, auto resolution with the real trace decision, one-warning latch'
    Sources     = @('gpu_timer_stats.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('UNDEBUG')
}
