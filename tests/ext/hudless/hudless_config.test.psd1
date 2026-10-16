@{
    Class       = 'cpu'
    Note        = 'S10 [HudLess] binder: keys/order/defaults, ini round-trip, clamps (dilation 0..4, threshold 0.001..1.0), Mode parse (unknown -> off), published mode / Wanted flag'
    Sources     = @('hudless_config.cpp', '{repo}/OptiScaler/ext/hudless/HudlessConfig.cpp', '{repo}/OptiScaler/ext/hudless/HudlessState.cpp',
                    '{repo}/OptiScaler/ext/hdr/HdrConfig.cpp', '{repo}/OptiScaler/ext/core/ExtConfig.cpp')
    IncludeDirs = @('{repo}/tests/ext/hdr/stubs', '{repo}/OptiScaler', '{repo}/external/simpleini')
    Defines     = @('UNDEBUG', 'NOMINMAX')
    Std         = 'c++latest'
}
