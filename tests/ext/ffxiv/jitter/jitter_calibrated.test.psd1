@{
    Class       = 'cpu'
    Note        = 'Game jitter calibrate-and-scale through the engine: the object never matches NGX (session 2), learning and adoption, key change, torn snapshots, stale verification, stale-block guard, exceptions, put-back and the log lines'
    Sources     = @('jitter_calibrated.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
