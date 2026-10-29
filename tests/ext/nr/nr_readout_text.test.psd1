@{
    Class       = 'cpu'
    Note        = 'NR card readouts: the NR time line, with n/a while nothing is measured (NrReadout.h)'
    Sources     = @('nr_readout_text.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
