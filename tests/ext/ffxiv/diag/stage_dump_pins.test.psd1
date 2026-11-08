@{
    Class       = 'cpu'
    Note        = 'Per-stage image dump, text pins: every stage calls the dump hook after its own pass returned and its guards are out of scope (exactly where the chain order says), registers its armed probe, the entry points cost one relaxed load, the copy sets no state, and no new key exists'
    Sources     = @('stage_dump_pins.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
