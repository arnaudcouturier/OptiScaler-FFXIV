@{
    Class       = 'cpu'
    Note        = 'Automatic ReShade depth: the pure decisions of the capture glue (render size as the per-axis minimum, output size and its fallbacks, a base that is never clamped, the character-preview filter, the gate order, the one-load early-out, the draw choice, the once-per-change capture line and rate-limited result line, the menu status line)'
    Sources     = @('reshade_depth_glue.cpp', '{repo}/OptiScaler/ext/reshade/ReShadeDepthCore.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Defines     = @('NOMINMAX', 'UNDEBUG')
    Std         = 'c++latest'
}
