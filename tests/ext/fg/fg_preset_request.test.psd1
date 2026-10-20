@{
    Class       = 'cpu'
    Note        = 'DLSS-G preset request model: setting ids, 0..3 mapping, driver-query and cached-reader answers, Auto restores the original (incl. not found), out-of-range rejected'
    Sources     = @('fg_preset_request.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
