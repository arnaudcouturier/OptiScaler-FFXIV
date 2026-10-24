@{
    Class       = 'cpu'
    Note        = 'Automatic ReShade depth: capture freshness (250 ms, 0-1 frames), macro value parsing, the first-and-every-1000th log rule, the late-registration planner over RegisterStatus and the depth event list'
    Sources     = @('reshade_depth_helpers.cpp', '{repo}/OptiScaler/ext/reshade/ReShadeDepthCore.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Defines     = @('NOMINMAX', 'UNDEBUG')
    Std         = 'c++latest'
}
