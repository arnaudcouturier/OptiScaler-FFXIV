@{
    Class       = 'warp'
    Note        = 'HDR10 HLSL color ramp, contrast, expansion, scene highlights, gamut controls and preview masking on D3D11 WARP'
    Prepare     = 'prepare_hdr10_shader.ps1'
    Sources     = @('hdr10_shader.cpp')
    IncludeDirs = @('{out}')
    Libs        = @('d3d11.lib', 'd3dcompiler.lib')
    Defines     = @('NOMINMAX')
}
