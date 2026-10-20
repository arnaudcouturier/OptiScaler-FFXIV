@{
    Class       = 'cpu'
    Note        = 'NR runtime catalog (NrRuntimeCatalog.h): folder-name parsing, version order, Automatic = newest (tie: legacy), explicit requests never fall back, size and hash text'
    Sources     = @('nr_runtime_catalog.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
}
