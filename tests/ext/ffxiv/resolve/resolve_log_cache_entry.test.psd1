@{
    Class       = 'cpu'
    Note        = 'resolver logging (fixed one-line-per-seam format, injectable sink, product sink via LOG macros), ResolveOnce caching, resolved-build zeroing, entry generator, unbound listing, known-build cross-check (table wins)'
    Sources     = @('resolve_log_cache_entry.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
