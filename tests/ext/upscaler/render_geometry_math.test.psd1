@{
    Class       = 'cpu'
    Note        = 'Pure per-axis-min render-geometry correction math (C2/C3), see PORT-LOG S7 C2/C3'
    Sources     = @('render_geometry_math_test.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('UNDEBUG')
}
