@{
    Class       = 'cpu'
    Note        = 'Dynamic MFG config: the four [DLSSG] keys bind with their defaults, PostLoad range checks, ini fragment documents them, real CustomOptional keeps the saved value under a volatile write'
    Sources     = @('dynamic_mfg_config.cpp', '{repo}/OptiScaler/ext/fg/FgConfig.cpp')
    IncludeDirs = @('{out}', 'mfg_cfg_stubs', '{repo}/OptiScaler')
    Prepare     = 'prepare_dynamic_mfg_config.ps1'
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
}
