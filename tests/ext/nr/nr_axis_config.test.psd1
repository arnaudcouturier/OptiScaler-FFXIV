@{
    Class       = 'cpu'
    Note        = 'DlssNr keys WorkingScaleX, WorkingScaleY, DepthAwareResolve against the real NrConfig::Bind (recording binder, real CustomOptional): each key is bound once to its own member, the defaults are today (follow, follow, off)'
    Sources     = @('nr_axis_config.cpp', '{repo}/OptiScaler/ext/nr/NrConfig.cpp')
    IncludeDirs = @('{out}', 'nr_cfg_stubs', '{repo}/tests/ext/fg/mfg_cfg_stubs', '{repo}/OptiScaler')
    Prepare     = 'prepare_nr_axis_config.ps1'
    Defines     = @('NOMINMAX')
}
