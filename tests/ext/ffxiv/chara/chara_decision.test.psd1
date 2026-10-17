@{
    Class       = 'cpu'
    Note        = 'CharaView draw classifier: prepass / G-buffer / finisher / consumer rules, geometry acceptance, finisher check, exhaustive combination check against an independent restatement, pass state'
    Sources     = @('chara_decision.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
