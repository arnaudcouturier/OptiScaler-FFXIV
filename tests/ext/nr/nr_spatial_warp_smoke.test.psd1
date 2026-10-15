@{
    Class       = 'warp'
    Note        = 'NR spatial warp shader against the CPU reference (D3D11 WARP, compiles dlssnr_spatial.hlsl at run time)'
    Prepare     = 'prepare_shaders.ps1'
    Sources     = @('nr_spatial_warp_smoke.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Libs        = @('d3d11.lib', 'd3dcompiler.lib')
    Defines     = @('NOMINMAX')
    Args        = @('{out}/shaders/dlssnr_spatial.hlsl')
}
