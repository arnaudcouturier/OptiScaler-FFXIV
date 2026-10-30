@{
    Class       = 'cpu'
    Note        = 'CharaView several-window diagnostics: per-output-entry counters (finishers, verdicts, evaluations, processed UI draws, failures), the two-finishers-of-one-image-in-one-frame detector, the images a UI draw sampled within 250 ms, the renders-per-game-frame histogram, the replay refusals with the untagged G-buffer shapes, and the burst reset'
    Sources     = @('chara_window_stats.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
