@{
    Class       = 'warp'
    Note        = 'CharaView preview NR compose passes on D3D11 WARP with the constants of CharaNrRecipe.h: encode pass-through keeps alpha, resolve of an unchanged answer is the original, an edit lands and keeps alpha, ApplyModel off and strength 0 give the clean image, zero-motion pass writes zeros'
    Prepare     = '..\..\nr\prepare_shaders.ps1'
    Sources     = @('chara_nr_warp.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Libs        = @('d3d11.lib', 'd3dcompiler.lib')
    Defines     = @('NOMINMAX')
    Args        = @('{out}/shaders/dlssnr.hlsl')
    Std         = 'c++20'
}
