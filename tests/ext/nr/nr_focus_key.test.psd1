@{
    Class       = 'cpu'
    Note        = 'DlssNr PauseOnFocusLoss (alias of DisableOnFocusLoss): migrated into the key at load and cleared, explicit false of the key wins, saved back as auto (real CustomOptional); binder, ini line and manifest wiring'
    Sources     = @('nr_focus_key.cpp')
    IncludeDirs = @('{out}', '{repo}/OptiScaler')
    Prepare     = '..\fg\prepare_dynamic_mfg_config.ps1'
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
}
