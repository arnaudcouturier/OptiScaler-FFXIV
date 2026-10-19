@{
    Class       = 'cpu'
    Note        = 'FFXIV menu tab: one row in upstream tabs[] right after Frame Gen with the build-match predicate from ext, and the seven FFXIV-gated cards registered for it (source text pins, comments stripped)'
    Sources     = @('menu_ffxiv_tab.cpp')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
