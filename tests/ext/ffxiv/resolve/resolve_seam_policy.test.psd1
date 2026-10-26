@{
    Class       = 'cpu'
    Note        = 'resolver core on synthetic images: decode modes, every predicate with decoys, multi-anchor policy truth table, expected-hit pairs, guard anchors, dependencies, EntryPatched, registry validation'
    Sources     = @('resolve_seam_policy.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
