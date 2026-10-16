@{
    Class       = 'cpu'
    Note        = 'S10 contract headers (ext/hudless/HudlessSource.h, HudlessGpu.h) compile standalone and keep their defaults; a fake IHudlessSource implements the interface'
    Sources     = @('hudless_api_headers.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('UNDEBUG', 'NOMINMAX')
}
