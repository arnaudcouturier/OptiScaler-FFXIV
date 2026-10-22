@{
    Class       = 'cpu'
    Note        = '[PACE] [FramePacing] keys through the real ini binding: Trace (no default) and HiddenPresentDoNotWait (default false) bind once, load/save round trip; FramePacing.ini documents them (uses the hdr test stubs)'
    Sources     = @('frame_pacing_config_test.cpp', '{repo}/OptiScaler/ext/core/ExtConfig.cpp', '{repo}/OptiScaler/ext/hdr/HdrConfig.cpp',
                    '{repo}/OptiScaler/ext/hudless/HudlessConfig.cpp', '{repo}/OptiScaler/ext/hudless/HudlessState.cpp')
    IncludeDirs = @('{repo}/tests/ext/hdr/stubs', '{repo}/OptiScaler', '{repo}/external/simpleini')
    Args        = @('{repo}')
    Std         = 'c++latest'
}
