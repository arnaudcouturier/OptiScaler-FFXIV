@{
    Class       = 'cpu'
    Note        = 'FG recorder writer cadence at 120 fps x 6 taps: at most about 12 writer loops a second and at most about 1.2 device flushes a second over a timed window (a flush per frame or a wake per tap fails here)'
    Sources     = @('fgrec_flush.cpp')
    IncludeDirs = @('stubs', '.', '{repo}/OptiScaler')
    Libs        = @('winmm.lib')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
