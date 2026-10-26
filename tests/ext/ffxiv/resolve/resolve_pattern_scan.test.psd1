@{
    Class       = 'cpu'
    Note        = 'resolver matcher: IDA pattern parsing with nibble masks, masked compare, chunked section scan (straddling, section bounds, executable-only, hit cap, unreadable image)'
    Sources     = @('resolve_pattern_scan.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
