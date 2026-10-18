@{
    Class       = 'cpu'
    Note        = 'Window mode: [Window] Mode default, ini binding round trip and the published cached mode (uses the hdr test stubs)'
    Sources     = @('window_config.cpp', '{repo}/OptiScaler/ext/core/ExtConfig.cpp', '{repo}/OptiScaler/ext/hdr/HdrConfig.cpp',
                    '{repo}/OptiScaler/ext/hudless/HudlessConfig.cpp', '{repo}/OptiScaler/ext/hudless/HudlessState.cpp')
    IncludeDirs = @('{repo}/tests/ext/hdr/stubs', '{repo}/OptiScaler', '{repo}/external/simpleini')
    Std         = 'c++latest'
}
