@{
    Class       = 'cpu'
    Note        = 'companion anchors pinned as text independently of GameBuild.h: pattern, section, hit counts, value rule, decode, entry flag, margin, predicates of every anchor, every binding, the batch16/batch32 mustDiffer group; no cross-group seam names'
    Sources     = @('resolve_companion_pins.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
