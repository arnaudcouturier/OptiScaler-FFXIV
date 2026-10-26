@{
    Class       = 'cpu'
    Note        = 'Upstream update check ([Hotfix] CheckForUpdate): off unless the ini or the menu turned it on; the override is never saved, and an explicit true survives Save Settings (real CustomOptional, real ExtConfig load/save hooks and glue; negative controls for each rule)'
    Sources     = @('update_check.cpp', '{repo}/OptiScaler/ext/core/UpdateCheck.cpp', '{repo}/OptiScaler/ext/core/ExtConfig.cpp',
                    '{repo}/OptiScaler/ext/hdr/HdrConfig.cpp', '{repo}/OptiScaler/ext/hudless/HudlessConfig.cpp',
                    '{repo}/OptiScaler/ext/hudless/HudlessState.cpp')
    IncludeDirs = @('{out}', '{repo}/tests/ext/hdr/stubs', '{repo}/OptiScaler', '{repo}/external/simpleini')
    Prepare     = 'prepare_update_check.ps1'
    Args        = @('{repo}')
    Std         = 'c++latest'
}
