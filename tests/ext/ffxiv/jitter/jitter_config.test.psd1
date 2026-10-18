@{
    Class       = 'cpu'
    Note        = 'JitterProbe and JitterCorrection config keys: [FFXIV] section, default off, ini round-trip, documented in the ini fragment'
    Sources     = @('jitter_config.cpp', '{repo}/OptiScaler/ext/core/ExtConfig.cpp', '{repo}/OptiScaler/ext/hdr/HdrConfig.cpp',
                    '{repo}/OptiScaler/ext/hudless/HudlessConfig.cpp', '{repo}/OptiScaler/ext/hudless/HudlessState.cpp')
    IncludeDirs = @('{repo}/tests/ext/hdr/stubs', '{repo}/OptiScaler', '{repo}/external/simpleini')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
