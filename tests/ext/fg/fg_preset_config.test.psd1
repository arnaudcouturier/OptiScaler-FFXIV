@{
    Class       = 'cpu'
    Note        = 'DLSS-G preset config: [DLSSG] RenderPreset binds once with default 0, PostLoad drops values outside 0..3, DLSSG.ini documents it'
    Sources     = @('fg_preset_config.cpp', '{repo}/OptiScaler/ext/fg/FgConfig.cpp')
    IncludeDirs = @('{out}', 'mfg_cfg_stubs', '{repo}/OptiScaler')
    Prepare     = 'prepare_dynamic_mfg_config.ps1'
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
}
