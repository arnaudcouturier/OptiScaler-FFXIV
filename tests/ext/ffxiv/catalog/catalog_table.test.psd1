@{
    Class       = 'cpu'
    Note        = 'Shader catalog table (UI 7, scene 5, lighting 9, chara 9, companion 4, nameplate 3; CRC values, owner mirrors, overlaps) and the partial-UI-catalog health rule (threshold 6 of 7 as a named constant, per call, never latched, required-mask slot)'
    Sources     = @('catalog_table.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
