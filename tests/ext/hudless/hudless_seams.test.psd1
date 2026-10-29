@{
    Class       = 'cpu'
    Note        = 'S10 seam wrappers (HudlessSeams.h): inert with both feature bits clear, the engine only with its own bit (the developer probe alone keeps it inert), the probe only with its own bit, overlay seam ignores D3D11 / null queue, text pin of the guards'
    Sources     = @('hudless_seams.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Libs        = @('d3d12.lib', 'dxgi.lib')
    Defines     = @('UNDEBUG', 'NOMINMAX')
    Args        = @('{repo}')
}
