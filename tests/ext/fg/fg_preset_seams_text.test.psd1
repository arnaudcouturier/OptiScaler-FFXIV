@{
    Class       = 'cpu'
    Note        = 'DLSS-G preset integration pins: the upstream seams and their order, Auto installs no hooks (only a switch calls Ensure), UI recomposition pause, menu text'
    Sources     = @('fg_preset_seams_text.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
