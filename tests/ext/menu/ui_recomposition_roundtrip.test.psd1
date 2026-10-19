@{
    Class       = 'cpu'
    Note        = 'UI recomposition combo (HudlessMenu.cpp) against the config: the declared default of [DLSSG] UIRecomposition equals what the engine does with an unset key, and Auto/On/Off survive Save Settings then a restart (the real FgConfig.cpp binding and the real CustomOptional, a Binder copy of the real Save/Load steps pinned by text; a negative control with the old default false must lose Off)'
    Sources     = @('ui_recomposition_roundtrip.cpp', '{repo}/OptiScaler/ext/fg/FgConfig.cpp')
    IncludeDirs = @('{out}', 'recomp_stubs', '{repo}/tests/ext/fg/mfg_cfg_stubs', '{repo}/OptiScaler')
    Prepare     = '..\fg\prepare_dynamic_mfg_config.ps1'
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
