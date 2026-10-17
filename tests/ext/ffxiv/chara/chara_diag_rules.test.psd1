@{
    Class       = 'cpu'
    Note        = 'CharaView DLSS diagnostics rules: file sizes of a dump, the 12 file / 100 MiB budget checked before writing, the two dump phases with the 5 s re-arm, half-float decode, motion statistics and the trace rate limiter'
    Sources     = @('chara_diag_rules.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
