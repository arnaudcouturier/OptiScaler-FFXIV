@{
    Class       = 'cpu'
    Note        = 'Draw diagnostic: draws.csv / cb.csv row formats, the frame marker ordering rule (tone map and first UI draw once, upscale boundary on the next row), the run folder naming and pruning'
    Sources     = @('draw_diag_format.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Std         = 'c++20'
}
