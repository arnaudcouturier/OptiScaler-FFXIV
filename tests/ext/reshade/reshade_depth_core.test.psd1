@{
    Class       = 'cpu'
    Note        = 'Automatic ReShade depth: the binder with a fake ReShade backend that models the single DEPTH binding and freed views (gate order, consumer window, trust of the depth input, restore only if still ours, reload mid pass, dirty runtimes that keep their references and retry, per-handle balance, manual transform macros and their cache, fault injection on every backend call, no allocation on refusals, threads)'
    Sources     = @('reshade_depth_core.cpp', '{repo}/OptiScaler/ext/reshade/ReShadeDepthCore.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Defines     = @('NOMINMAX', 'UNDEBUG')
    Std         = 'c++latest'
}
