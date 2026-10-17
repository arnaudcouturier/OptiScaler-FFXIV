@{
    Class       = 'cpu'
    Note        = 'S10 E: ApplyResult selection with a fake ReShade backend (off, not registered, runtime recreated, effects disabled, zero techniques, refusals, exception latch, once per frame, backbuffer references released) and the ReShadePresets parser'
    Sources     = @('reshade_boundary_apply.cpp', '{repo}/OptiScaler/ext/reshade/ReShadeBoundaryCore.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Defines     = @('NOMINMAX', 'UNDEBUG')
    Std         = 'c++latest'
}
