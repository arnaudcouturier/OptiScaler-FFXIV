@{
    Class       = 'warp'
    Note        = 'NR skin / residual / motion / HDR-carrier compose passes (D3D11 WARP, compiles dlssnr.hlsl at run time)'
    Prepare     = 'prepare_shaders.ps1'
    Sources     = @('nr_skin_shader_smoke.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Libs        = @('d3d11.lib', 'd3dcompiler.lib')
    Defines     = @('NOMINMAX')
    Args        = @('{out}/shaders/dlssnr.hlsl')
}
