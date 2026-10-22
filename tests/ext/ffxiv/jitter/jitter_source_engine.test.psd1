@{
    Class       = 'cpu'
    Note        = 'Jitter source fix next to calibrate-and-scale: the camera object may hold up to 0.5 / strength (above the old 0.5 pixel snapshot guard), the probe reads it and says the fix is active, the stood-down engine writes and reads nothing and keeps its ratio for the fall-back'
    Sources     = @('jitter_source_engine.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
