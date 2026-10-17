@{
    Class       = 'cpu'
    Note        = 'CharaView isolated DLSS availability gate: every reason (config off, latched, no NGX module, DLSS off, D3D11 NGX not initialised, entry points, device mismatch), their precedence and the one-line-per-reason log limit'
    Sources     = @('chara_dlss_gate.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
