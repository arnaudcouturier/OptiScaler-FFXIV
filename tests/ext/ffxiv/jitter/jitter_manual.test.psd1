@{
    Class       = 'cpu'
    Note        = 'Game jitter manual scale ([FFXIV] JitterScaleOverride): the experiment value takes precedence over the calibration, 0 is off, invalid values are ignored, same application path as the calibrated scale'
    Sources     = @('jitter_manual.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
