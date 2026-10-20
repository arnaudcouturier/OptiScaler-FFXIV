@{
    Class       = 'cpu'
    Note        = 'NR model version picker text (NrRuntimeText.h): combo entries with version, size and short SHA-256, "(not installed)" entry, status lines (unavailable, switching, restart-only notes from real state), pinned'
    Sources     = @('nr_runtime_text.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
}
