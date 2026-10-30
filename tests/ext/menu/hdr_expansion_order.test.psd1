@{
    Class       = 'cpu'
    Note        = 'FFXIV HDR card row order: Highlight expansion keeps its Reset and tooltip, the UI expansion row follows with its own, disabled under the legacy composition (source text pins, comments stripped)'
    Sources     = @('hdr_expansion_order.cpp')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
