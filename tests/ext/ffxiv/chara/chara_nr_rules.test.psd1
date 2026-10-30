@{
    Class       = 'cpu'
    Note        = 'CharaView preview NR rules: availability reasons and their precedence, one log line per change, the independent SR and NR latches, stream binding, linger and release only after the GPU finished, history reset, the D3D11/D3D12 fence hand-off order on fakes (no D3D11 wait after a failure), the least-recently-used stream eviction, the late result pick, cost logging'
    Sources     = @('chara_nr_rules.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
