@{
    Class       = 'cpu'
    Note        = 'jitter-source seams against the previous build B: only B-read / B-read-partial rows (generator prologue 20 B, phases 48 B with maskB, mov edx,-1) as PREFIX evidence of the strict patterns; derived rows refused; with every B row planted no seam resolves and no field holds a C value (the 0x25f610 reuse trap)'
    Sources     = @('resolve_jitter_b_fixture.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
