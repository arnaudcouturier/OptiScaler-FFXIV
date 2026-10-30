@{
    Class       = 'cpu'
    Note        = 'The simulation-tick anchors on committed bytes only: the seam/kind/anchor/binding pins, both builds'' counter offsets decoded from the same instruction text (2024 and 2026), the install layout check refusing a mixed pair, and the decoys - a prologue copy in another function filtered by SameFunctionAs (inside the same function: Ambiguous), one broken interior block keeping the tick usable and costing only the two counter offsets, both broken NotFound, an out-of-range counter dropped by ValueIn, an instance load into initialized .data instead of the bss tail dropped by TargetIn, and the wildcarded short-branch displacements of the tail'
    Sources     = @('resolve_latency_old_build.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
