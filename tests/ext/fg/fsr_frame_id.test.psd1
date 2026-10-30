@{
    Class       = 'cpu'
    Note        = 'FSR FG frame IDs: the ID rises by exactly 1 per dispatch across forward jumps, backwards game counters and repeats, one dispatch asking twice reads one ID, the recorded buffer slot comes back per ID (and -1 when unknown), old IDs are forgotten; plus wiring pins (both provider ID writes and both callbacks through the seam, the other providers untouched, the key and its ini line)'
    Sources     = @('fsr_frame_id.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
}
