@{
    Class       = 'cpu'
    Note        = 'NR hotkey wiring pins: bound keys, ini lines, Keybinds rows, single-write press handlers, shared toggle rule'
    Sources     = @('nr_hotkeys_wiring.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
    Std         = 'c++20'
}
