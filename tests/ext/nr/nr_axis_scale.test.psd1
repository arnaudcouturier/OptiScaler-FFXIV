@{
    Class       = 'cpu'
    Note        = 'NR per-axis model resolution: the effective scale of each axis, the geometry and layout built from two scales (equal axes behave as before, X and Y never swap), the per-axis motion-vector scale and the guide matching rule (NrModelSize.h, DlssNr_Spatial.h, FoveatedWarp.h, DlssNr_Guides.h)'
    Sources     = @('nr_axis_scale.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
