@{
    Class       = 'cpu'
    Note        = 'Draw diagnostic wiring pins: one relaxed load on the draw path, reflection at shader creation, upscale boundary and Present marks, one capture per session, defaults off, ini-only keys'
    Sources     = @('draw_diag_pins.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
