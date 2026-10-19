@{
    Class       = 'cpu'
    Note        = 'Menu layout table: the order of the fork cards does not depend on registration order (every permutation of a small set, shuffles of the real table), fallbacks for unlisted cards and unknown anchors, expected order of each tab, MergeBoxes uses the table and the registration index'
    Sources     = @('menu_layout.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
