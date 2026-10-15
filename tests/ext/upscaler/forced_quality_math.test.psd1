@{
    Class       = 'cpu'
    Note        = 'Pure slack math + EffectivePerfQuality/ApplyForcedQuality decisions (C1/C5), see PORT-LOG S7 C1/C5'
    Sources     = @('forced_quality_math_test.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('UNDEBUG')
}
