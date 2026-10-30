@{
    Class       = 'cpu'
    Note        = '[BRIDGE] [Bridge] FlushAfterSignal through the real ini binding: one bool key, default false, load/save round trip, PostLoad publishes the cached copy the hand-over reads, Bridge.ini documents it (uses the hdr test stubs)'
    Sources     = @('bridge_config_test.cpp', '{repo}/OptiScaler/ext/core/ExtConfig.cpp', '{repo}/OptiScaler/ext/hdr/HdrConfig.cpp',
                    '{repo}/OptiScaler/ext/hudless/HudlessConfig.cpp', '{repo}/OptiScaler/ext/hudless/HudlessState.cpp')
    IncludeDirs = @('{repo}/tests/ext/hdr/stubs', '{repo}/OptiScaler', '{repo}/external/simpleini')
    Args        = @('{repo}')
    Std         = 'c++latest'
}
