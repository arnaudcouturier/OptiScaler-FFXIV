@{
    Class       = 'cpu'
    Note        = 'CharaView Halton (2,3) jitter: 16 phases with exact values, viewport shift, alpha uv offset'
    Sources     = @('chara_jitter.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
