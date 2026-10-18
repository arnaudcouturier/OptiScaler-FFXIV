@{
    Class       = 'cpu'
    Note        = 'CharaView Halton (2,3) jitter: 32 phases tied to the preview scale, exact values against an independent loop, coverage, RecommendedPhases against the FSR rule, viewport shift, alpha uv offset'
    Sources     = @('chara_jitter.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
