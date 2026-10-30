@{
    Class       = 'cpu'
    Note        = 'The simulation-tick probe rules with injected events: percentile ranks, the 0.25 ms histogram edges and its overflow floor, the signed split, the 10 s window and its restart on a clock that steps back, the frame-number pairing, the two pairing rules (the present inside the tick on its thread with the present wait as W, and the two-thread shape with the queue wait), the window metrics (period, body, age, W, queue, slack, sleep->tick, the Reflex sleep duration, pairing health, the pairing word, the thread table), and the exact text of the logged line including every n/a'
    Sources     = @('tick_probe.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
}
