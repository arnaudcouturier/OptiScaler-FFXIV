@{
    Class       = 'warp'
    Note        = 'PreSR BoundResidual pass (D3D11 WARP, compiles dlssnr.hlsl at run time)'
    Prepare     = 'prepare_shaders.ps1'
    Sources     = @('nr_presr_bounds_shader.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Libs        = @('d3d11.lib', 'd3dcompiler.lib')
    Defines     = @('NOMINMAX')
    Args        = @('{out}/shaders/dlssnr.hlsl')
}
