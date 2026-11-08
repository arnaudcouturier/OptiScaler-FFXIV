@{
    Class       = 'cpu'
    Note        = 'Draw diagnostic: the light and camera constants are sampled at most once a second (the clock of the last read is remembered, a draw inside the second is refused)'
    Sources     = @('draw_diag_sample.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Libs        = @('user32.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
