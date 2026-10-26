@{
    Class       = 'cpu'
    Note        = 'Keys: [FFXIV] FfxivLegacyGameBuild / FfxivUnknownBuildHooks / FfxivForceUnknownBuild default off, bound once, ini round trip, documented in the ini fragment with "No menu control", listed in the ini-only keys, default GateSettings all false'
    Sources     = @('gate_keys.cpp', '{repo}/OptiScaler/ext/core/ExtConfig.cpp', '{repo}/OptiScaler/ext/hdr/HdrConfig.cpp',
                    '{repo}/OptiScaler/ext/hudless/HudlessConfig.cpp', '{repo}/OptiScaler/ext/hudless/HudlessState.cpp')
    IncludeDirs = @('{repo}/tests/ext/hdr/stubs', '{repo}/OptiScaler', '{repo}/external/simpleini')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
