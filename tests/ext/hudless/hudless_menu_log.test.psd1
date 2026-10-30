@{
    Class       = 'cpu'
    Note        = 'HUD-less card slider logging: a change settles (500 ms without another change) into exactly one line with the final value, a drag writes only where it came to rest, a repeated same value is not a move and a backwards clock does not log early'
    Sources     = @('hudless_menu_log.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
