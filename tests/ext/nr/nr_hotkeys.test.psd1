@{
    Class       = 'cpu'
    Note        = 'NR hotkey step and toggle rules (NrHotkeys.h): 5% grid steps, clamps, off-grid snap, toggle no-op'
    Sources     = @('nr_hotkeys.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
    Std         = 'c++20'
}
