@{
    Class       = 'cpu'
    Note        = 'HDR config defaults, INI binding round-trip, hexadecimal screenshot key, retired keys and one-time scene hook'
    Sources     = @('hdr_config.cpp', '{repo}/OptiScaler/ext/hdr/HdrConfig.cpp', '{repo}/OptiScaler/ext/core/ExtConfig.cpp',
                    '{repo}/OptiScaler/ext/hudless/HudlessConfig.cpp', '{repo}/OptiScaler/ext/hudless/HudlessState.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler', '{repo}/external/simpleini')
    Std         = 'c++latest'
}
