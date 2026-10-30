@{
    Class       = 'cpu'
    Note        = 'jitter-source seams on committed bytes: every anchor pinned as a literal, the build-C layout resolves to the table (entry text, guard arrays, return addresses), the safety gap (the real VerifyAnchors over a resolved unknown build refuses when any consumer-read byte is flipped, accepts masked ones)'
    Sources     = @('resolve_jitter_anchors.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
