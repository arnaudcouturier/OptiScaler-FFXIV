@{
    Class       = 'warp'
    Note        = 'CharaView DLSS helper passes on D3D11 WARP: chara_guides.hlsl compiled from the repository and run through CharaGuides.cpp: depth copy (24-bit view), signed motion minus the camera offset with NaN/inf to zero and the +-2 clamp, colour copy channel order, alpha taken from the native image at uv + jitter / size, and the private context state restoring the game pipeline state'
    Sources     = @('chara_guides_warp.cpp', '{repo}/OptiScaler/ext/ffxiv/chara/CharaGuides.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Libs        = @('d3d11.lib', 'dxgi.lib', 'd3dcompiler.lib')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
