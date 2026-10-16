@{
    Class       = 'cpu'
    Note        = 'S10 D0: armed one-frame probe state machine with a fake ReShade/D3D11 backend (skip methods, verdicts, refusals, exception latch, timeouts)'
    Sources     = @('reshade_probe_flow.cpp', '{repo}/OptiScaler/ext/reshade/ReShadeProbeCore.cpp', '{repo}/OptiScaler/ext/reshade/ReShadeReport.cpp', '{repo}/OptiScaler/ext/reshade/ReShadeProbe.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Defines     = @('NOMINMAX', 'UNDEBUG')
    Std         = 'c++latest'
}
