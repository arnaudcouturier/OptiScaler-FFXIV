@{
    Class       = 'cpu'
    Note        = 'DLSS-G preset guards: structural scan of the nvngx_dlssg image on a synthetic image (found, every guard fails closed on its own, frame-size independent), provider choice, hash and version gates'
    Sources     = @('fg_preset_guards.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
