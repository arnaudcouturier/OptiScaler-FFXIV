@{
    Class       = 'cpu'
    Note        = 'CharaView DLSS slot life cycle with an injected clock: finish verdicts (size, stale, guides, rebuild, failed), first evaluation un-jittered and reset, 250 ms gap reset, query-gated 2000 ms release, latch drain, jitter feed'
    Sources     = @('chara_dlss_lifecycle.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
