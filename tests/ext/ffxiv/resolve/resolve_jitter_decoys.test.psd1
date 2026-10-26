@{
    Class       = 'cpu'
    Note        = 'jitter-source seams (P2c): every predicate removes its planted decoy (getter position, SameFunctionAs, CallsSeam, TargetIs, TargetIn(Bss)), exact ValueIn / ValueEquals ranges, EntryPatched over each Function entry (fields zero, weak dependents), the two Detoured functions can never coincide (mustDiffer), the two device-size template copies'
    Sources     = @('resolve_jitter_decoys.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
