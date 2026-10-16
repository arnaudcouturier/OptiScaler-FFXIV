@{
    Class       = 'cpu'
    Note        = 'S10 seam wrappers (HudlessSeams.h): inert with the feature bit clear (mode off), exactly one call with it set, overlay seam ignores D3D11 / null queue'
    Sources     = @('hudless_seams.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Libs        = @('d3d12.lib', 'dxgi.lib')
    Defines     = @('UNDEBUG', 'NOMINMAX')
}
