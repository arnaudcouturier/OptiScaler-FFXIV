@{
    Class       = 'cpu'
    Note        = 'Source-text pins of the game selector warning: the exact build logs the match, a name-recognised unknown build logs the rule, and the exe-name FG quirk warns once naming both halves'
    Sources     = @('game_select_wiring.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
    Std         = 'c++20'
}
