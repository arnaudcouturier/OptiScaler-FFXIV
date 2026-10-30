@{
    Class       = 'warp'
    Note        = 'Native HDR preview coverage mask, blend/depth-state restoration, atlas opacity, frame sealing and the Diagnostics-gated census on D3D11 WARP'
    Sources     = @('hdr_preview.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Libs        = @('d3d11.lib', 'd3d12.lib', 'dxgi.lib', 'd3dcompiler.lib')
    Defines     = @('NOMINMAX')
}
