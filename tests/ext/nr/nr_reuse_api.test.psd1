@{
    Class       = 'cpu'
    Note        = 'NR bottleneck-reuse driver wrapper (LaunchEx, BeginEvaluate) on the sliced production code, with two negative controls (needs external/nvapi headers)'
    Runner      = 'run_nr_reuse_api.ps1'
    Sources     = @('nr_reuse_api.cpp')
}
