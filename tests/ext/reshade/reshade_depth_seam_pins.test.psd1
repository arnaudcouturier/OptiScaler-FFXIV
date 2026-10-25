@{
    Class       = 'cpu'
    Note        = 'Automatic ReShade depth, source pins: one call right after the successful Evaluate and before the feature table lock, declared once in ReShadeSeams.h and included by the facade, the one-load first statement, config load/reload only store, the NGX shutdown calls'
    Sources     = @('reshade_depth_seam_pins_test.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('UNDEBUG')
}
