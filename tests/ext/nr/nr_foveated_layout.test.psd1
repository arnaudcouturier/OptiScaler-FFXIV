@{
    Class       = 'cpu'
    Note        = 'NR foveated profile in the spatial layout: settings/layout equality for every field, output-to-frame sizes, constants per profile, geometry and the applied layout while a change waits (DlssNr_Spatial.h)'
    Sources     = @('nr_foveated_layout.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
