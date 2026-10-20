@{
    Class       = 'cpu'
    Note        = 'Game jitter calibrate-and-scale: snapshot acceptance, ratio learning, adoption, verification, replacement, key change, stale clock, scale arithmetic and the stale-block guard on fake snapshots'
    Sources     = @('jitter_calibration.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
