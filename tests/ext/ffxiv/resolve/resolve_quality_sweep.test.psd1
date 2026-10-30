@{
    Class       = 'cpu'
    Note        = 'Quality group, generated from the registry: every masked and undecoded byte is in a short allow-list with a reason (kills lockstep over-masking), every fixed byte is flipped once (anchor not Unique, seam not Found, shape seam NotFound with a ZERO Guard), the first 14 bytes of each hooked entry are fixed'
    Sources     = @('resolve_quality_sweep.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
