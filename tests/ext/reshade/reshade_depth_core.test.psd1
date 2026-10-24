@{
    Class       = 'cpu'
    Note        = 'Automatic ReShade depth: the binder with a fake ReShade backend (every gate and its order, bind and restore only if still ours, view reference balance, depth input names and decoys, bufready_depth, manual transform macros and their cache, once per frame, runtime lifetime, fail-open fault injection on every backend call)'
    Sources     = @('reshade_depth_core.cpp', '{repo}/OptiScaler/ext/reshade/ReShadeDepthCore.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Defines     = @('NOMINMAX', 'UNDEBUG')
    Std         = 'c++latest'
}
