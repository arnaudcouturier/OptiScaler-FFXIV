@{
    Class       = 'cpu'
    Note        = 'S10 contract headers (ext/hudless/HudlessSource.h, HudlessGpu.h, HudlessState.h) compile standalone and keep their defaults; a fake IHudlessSource implements the interface; the Wanted() flag works'
    Sources     = @('hudless_api_headers.cpp', '{repo}/OptiScaler/ext/hudless/HudlessState.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Defines     = @('UNDEBUG', 'NOMINMAX')
}
