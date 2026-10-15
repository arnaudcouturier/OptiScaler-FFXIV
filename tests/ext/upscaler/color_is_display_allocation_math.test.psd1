@{
    Class       = 'cpu'
    Note        = 'Pure forced-quality colour-scale decision math (C4), see PORT-LOG S7 C4'
    Sources     = @('color_is_display_allocation_math_test.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('UNDEBUG')
}
