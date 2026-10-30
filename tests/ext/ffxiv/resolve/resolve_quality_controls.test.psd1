@{
    Class       = 'cpu'
    Note        = 'Quality group controls on committed build-C bytes: every predicate removes its planted decoy (scan and guard anchors), EntryPatched for the four entry anchors (also the relaxed retry alone), consumer-relied bytes pinned (flip tests), masked bytes irrelevant, offsets follow the code or are refused, failure chain'
    Sources     = @('resolve_quality_controls.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
