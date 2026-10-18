@{
    Class       = 'warp'
    Note        = 'CharaView preview NR debug tint on D3D11 WARP: the bring-up result (RGBA16F) is tinted cyan by one extra pass into a private target with the alpha kept and the result itself untouched, once per result serial, per image, rebuilt on a size change, released when unused, no tint claimed without the cyan shader; the alpha pass makes cyan for an NR colour, magenta for plain SR, plain without the tint'
    Sources     = @('chara_nr_tint_warp.cpp', '{repo}/OptiScaler/ext/ffxiv/chara/CharaNrTint.cpp', '{repo}/OptiScaler/ext/ffxiv/chara/CharaGuides.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Libs        = @('d3d11.lib', 'dxgi.lib', 'd3dcompiler.lib')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
