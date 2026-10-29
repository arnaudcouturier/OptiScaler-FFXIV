@{
    Class       = 'warp'
    Note        = 'NR community shaders (resize guides, resolve, spatial, guide match) on D3D11 WARP, compiled at run time from the three HLSL sources'
    Prepare     = 'prepare_shaders.ps1'
    Sources     = @('nr_community_shader.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Libs        = @('d3d11.lib', 'd3dcompiler.lib')
    Defines     = @('NOMINMAX')
    Args        = @('{out}/shaders/dlssnr.hlsl', '{out}/shaders/dlssnr_spatial.hlsl', '{out}/shaders/dlssnr_guide_match.hlsl', '{out}/shaders/dlssnr_v040_reference.hlsl')
}
