@{
    Class       = 'warp'
    Note        = 'NR "Linear light + composed" (ReversibleMode 5): encode/resolve behaviour (D3D11 WARP, compiles dlssnr.hlsl at run time)'
    Prepare     = 'prepare_shaders.ps1'
    Sources     = @('nr_linear_light_shader.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Libs        = @('d3d11.lib', 'd3dcompiler.lib')
    Defines     = @('NOMINMAX')
    Args        = @('{out}/shaders/dlssnr.hlsl')
}
