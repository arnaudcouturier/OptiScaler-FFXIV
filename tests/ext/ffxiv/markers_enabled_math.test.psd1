@{
    Class       = 'cpu'
    Note        = 'Pure IFgMarkers Enabled() decision math (S7 IFgMarkers), see PORT-LOG S7 IFgMarkers'
    Sources     = @('markers_enabled_math_test.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('UNDEBUG')
}
