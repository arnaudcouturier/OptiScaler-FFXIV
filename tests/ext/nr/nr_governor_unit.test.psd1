@{
    Class       = 'cpu'
    Note        = 'NR frame-rate governor pure decision core (ext/nr/NrGovernor.h) on a simulated clock: step cadence and floor, hold bands and edges, up probes with verdicts and back-off, first steps and ceilings, stand-downs, transients, VRAM hold, counter freshness, pass gaps, transition windows, smoothing, multiplier, status line'
    Sources     = @('nr_governor_unit.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Std         = 'c++20'
}
