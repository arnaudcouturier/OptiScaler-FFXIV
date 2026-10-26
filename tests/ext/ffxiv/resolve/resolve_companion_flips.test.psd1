@{
    Class       = 'cpu'
    Note        = 'companion over-masking guard: a literal mask map per anchor (exact masked byte set, classified by capstone against the executable) compared with bit-by-bit flips of every anchor span on the committed bytes, and the consumer-relied bytes (by RVA) that must stay fixed in every anchor that spans them, with image-level flips'
    Sources     = @('resolve_companion_flips.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
