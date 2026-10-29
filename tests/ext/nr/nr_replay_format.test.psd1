@{
    Class       = 'cpu'
    Note        = 'NR frame-replay set: manifest writer pinned byte for byte, reader refusals (version, missing keys, truncation, consistency), hashing that ignores row padding, copy formats'
    Sources     = @('nr_replay_format.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
