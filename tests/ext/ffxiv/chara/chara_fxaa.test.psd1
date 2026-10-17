@{
    Class       = 'warp'
    Note        = 'CharaView spatial AA on D3D11 WARP: the real FxaaPass with the real shader (alpha rule, flat early-out, edge smoothing, 2x output, sRGB views), the game pipeline state is restored, the swapped t0 is used by the draw and put back'
    Prepare     = 'prepare_fxaa.ps1'
    Sources     = @('chara_fxaa.cpp', '{repo}/OptiScaler/ext/ffxiv/chara/CharaEnhancePass.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler', '{out}')
    Libs        = @('d3d11.lib', 'dxgi.lib', 'd3dcompiler.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
