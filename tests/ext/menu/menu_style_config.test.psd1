@{
    Class       = 'cpu'
    Note        = 'Menu style: [Menu] Style default, ini binding round trip and unknown values (uses the hdr test stubs)'
    Sources     = @('menu_style_config.cpp', '{repo}/OptiScaler/ext/core/ExtConfig.cpp', '{repo}/OptiScaler/ext/hdr/HdrConfig.cpp',
                    '{repo}/OptiScaler/ext/hudless/HudlessConfig.cpp', '{repo}/OptiScaler/ext/hudless/HudlessState.cpp')
    IncludeDirs = @('{repo}/tests/ext/hdr/stubs', '{repo}/OptiScaler', '{repo}/external/simpleini')
    Std         = 'c++latest'
}
