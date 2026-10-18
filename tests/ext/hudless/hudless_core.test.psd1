@{
    Class       = 'cpu'
    Note        = 'S10 engine core driven by a fake IHudlessSource and fake provider/GPU host: every fallback reason, seam sequencing, hysteresis, coverage guard incl. unknown, release and starvation, probing, SceneHdrOwnsFrame, mode off touches nothing, log lines; replays of the producer / source feedback loop'
    Sources     = @('hudless_core.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Defines     = @('UNDEBUG', 'NOMINMAX')
}
