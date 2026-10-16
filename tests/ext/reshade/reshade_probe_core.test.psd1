@{
    Class       = 'cpu'
    Note        = 'S10 D0: probe core without ReShade: runtime report text, F3 frame ledger, run folders/pruning, file report, session event flow with a fake backend'
    Sources     = @('reshade_probe_core.cpp', '{repo}/OptiScaler/ext/reshade/ReShadeProbeCore.cpp', '{repo}/OptiScaler/ext/reshade/ReShadeReport.cpp', '{repo}/OptiScaler/ext/reshade/ReShadeProbe.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Defines     = @('NOMINMAX', 'UNDEBUG')
    Std         = 'c++latest'
}
