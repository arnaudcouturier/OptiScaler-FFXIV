@{
    Class       = 'cpu'
    Note        = 'Source-text pins for the catalog wiring (count in the always-run creation hook with the hook''s own CRC and no Armed/Wanted condition, UI guard returns NotThisFrame after Mode and before the live source, Present tick before the Armed test, time base before the detour commit) and for the lighting/nameplate CRC literals the table mirrors; inline glue against the global ledger'
    Sources     = @('catalog_wiring_pins.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Args        = @('{repo}')
}
