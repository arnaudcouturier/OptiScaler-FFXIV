@{
    Class       = 'cpu'
    Note        = 'NR settle decision: keep / wait / history reset / rebuild / immediate rebuild, the 500 ms boundary, the layout consumers see while waiting, partial park (NrSettle.h)'
    Sources     = @('nr_settle.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
