@{
    Class       = 'warp'
    Note        = 'CharaView spatial AA integration and flood test on D3D11 WARP: the real CharaEnhance::Around with a fake table; only the preview renderer''s equipment output (entry 0, exact size) is filtered, the draw runs once with the filtered t0 and the game''s t0 is bound again, everything else and every unsupported case stays native and untouched'
    Prepare     = 'prepare_fxaa.ps1'
    Sources     = @('chara_enhance.cpp', '{repo}/OptiScaler/ext/ffxiv/chara/CharaEnhance.cpp', '{repo}/OptiScaler/ext/ffxiv/chara/CharaEnhancePass.cpp', '{repo}/OptiScaler/ext/ffxiv/chara/CharaFlood.cpp',
                    '{repo}/OptiScaler/ext/core/ExtConfig.cpp', '{repo}/OptiScaler/ext/hdr/HdrConfig.cpp',
                    '{repo}/OptiScaler/ext/hudless/HudlessConfig.cpp', '{repo}/OptiScaler/ext/hudless/HudlessState.cpp')
    IncludeDirs = @('stubs', '{repo}/tests/ext/hdr/stubs', '{repo}/OptiScaler', '{repo}/external/simpleini', '{out}')
    Libs        = @('d3d11.lib', 'dxgi.lib', 'd3dcompiler.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
