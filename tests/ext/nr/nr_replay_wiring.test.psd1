@{
    Class       = 'cpu'
    Note        = 'NR replay capture wiring: the key (default false, ini-only), what a capture trigger does with it, recording order around a dispatch, the on-demand capture''s before image is the untouched game frame, dispatch kind names'
    Sources     = @('nr_replay_wiring.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
}
