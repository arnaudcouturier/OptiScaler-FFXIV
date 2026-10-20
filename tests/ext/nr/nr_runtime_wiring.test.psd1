@{
    Class   = 'cpu'
    Note    = 'NR runtime picker wiring in production sources: no recursive model search left, PollRuntime before the failure latch, park via ParkNrFeature, pinned paths use the startup selection'
    Runner  = 'run_nr_runtime_wiring.ps1'
    Sources = @('nr_runtime_wiring.cpp')
}
