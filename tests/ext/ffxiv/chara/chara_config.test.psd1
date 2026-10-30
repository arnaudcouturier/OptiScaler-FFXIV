@{
    Class       = 'cpu'
    Note        = 'CharaView config: fifteen keys with their sections and defaults (PreviewUpscale true, the budget off with a count of two), ini round-trip, ini fragments carry every key'
    Sources     = @('chara_config.cpp', '{repo}/OptiScaler/ext/core/ExtConfig.cpp', '{repo}/OptiScaler/ext/hdr/HdrConfig.cpp',
                    '{repo}/OptiScaler/ext/hudless/HudlessConfig.cpp', '{repo}/OptiScaler/ext/hudless/HudlessState.cpp')
    IncludeDirs = @('{repo}/tests/ext/hdr/stubs', '{repo}/OptiScaler', '{repo}/external/simpleini')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
