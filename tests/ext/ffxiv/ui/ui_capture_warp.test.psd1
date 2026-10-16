@{
    Class       = 'warp'
    Note        = 'S10-A: boundary copy equals the render target bound at the candidate draw (before the draw), tagged / pre-hook shader handling, in-Present and foreign-thread draws, render target classification, capture refusals (D3D11 WARP)'
    Sources     = @('ui_capture_warp.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Libs        = @('d3d11.lib', 'dxgi.lib', 'd3dcompiler.lib')
    Defines     = @('NOMINMAX')
}
