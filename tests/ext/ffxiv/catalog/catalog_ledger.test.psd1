@{
    Class       = 'cpu'
    Note        = 'Per-catalog hit accounting: masks per catalog (overlaps, vertex vs pixel), first-seen time/present/order once, fingerprint once, uncatalogued sample (dedupe, hard cap 256, zero heap allocations), UI health per call, summary / first-seen / fingerprint / missing / near-miss log lines (settle, log once, caps), threads'
    Sources     = @('catalog_ledger.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
