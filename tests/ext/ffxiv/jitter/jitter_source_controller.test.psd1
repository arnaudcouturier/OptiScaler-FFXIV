@{
    Class       = 'cpu'
    Note        = 'Jitter source fix, boundary controller on a fake parameter block: cost with the key off, the told-offset write (X then Y, reads first), pairing miss passes through, stand-down of calibrate-and-scale, fall-back by verification or unpaired run, key switched off with in-flight frames, decision reasons and log lines'
    Sources     = @('jitter_source_controller.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
