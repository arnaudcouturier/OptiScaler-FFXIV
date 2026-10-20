@{
    Class       = 'cpu'
    Note        = 'DLSS-G preset readout: tolerant parser of the NVIDIA [ngxLog] preset lines on synthetic lines, readout state, candidate-line rate limit, own lines ignored'
    Sources     = @('fg_preset_log.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
