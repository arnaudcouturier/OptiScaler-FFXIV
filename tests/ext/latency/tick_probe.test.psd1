@{
    Class       = 'cpu'
    Note        = 'The simulation-tick probe rules with injected events: percentile ranks, the 0.25 ms histogram edges and its overflow floor, the signed split, the 10 s window and its restart on a clock that steps back, the pairing search finding the real lag of a two-frame pipeline and keeping the lowest lag on a tie, the window metrics (period, body, age, queue wait, sleep->tick with the stale-sleep rule, pairing health, the thread table), and the exact text of the logged line including every n/a'
    Sources     = @('tick_probe.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
}
