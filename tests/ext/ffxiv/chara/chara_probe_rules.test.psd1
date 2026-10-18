@{
    Class       = 'cpu'
    Note        = 'CharaView probe rules: derived-texture tracker (3 hop cap, 3 s expiry, refresh, full set, dest == source refused, copies off), log case caps, rate counters, 40 file / 300 MiB budget, live command parser (garbage, NUL, oversize) and the PNG writer (round trip through an independent decoder, chunk CRCs, IHDR, Adler-32)'
    Sources     = @('chara_probe_rules.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
