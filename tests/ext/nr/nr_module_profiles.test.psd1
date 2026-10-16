@{
    Class       = 'cpu'
    Note        = 'NR CUDA module fingerprint table: known module keeps reuse+hybrid, unknown gets neither, reuse-only rows never enable hybrid (DlssNrModuleProfiles.h)'
    Sources     = @('nr_module_profiles.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
}
