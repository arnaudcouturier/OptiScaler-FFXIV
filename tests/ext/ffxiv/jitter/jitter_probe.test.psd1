@{
    Class       = 'cpu'
    Note        = 'Game jitter probe and correction: guarded chain read on fake memory images, applied/reported ratios, sign and scale cases, rate limit, episodes, correction decision and text'
    Sources     = @('jitter_probe.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
