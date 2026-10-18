@{
    Class       = 'warp'
    Note        = 'CharaView dispatcher (the real CharaDispatch.cpp) on D3D11 WARP with the DLSS/NR plumbing, the probe and the table accessor as recording test doubles: the native draw runs exactly once on every path and route, a pass is closed by an accepted finisher and not cut by the frame callback, PreviewDLSS without a runnable path falls back to the spatial AA, slots are cleaned up after the option is switched off, the unhook thread never tears down'
    Prepare     = 'prepare_fxaa.ps1'
    Sources     = @('chara_dispatch.cpp', '{repo}/OptiScaler/ext/ffxiv/chara/CharaDispatch.cpp', '{repo}/OptiScaler/ext/ffxiv/chara/CharaEnhance.cpp', '{repo}/OptiScaler/ext/ffxiv/chara/CharaEnhancePass.cpp', '{repo}/OptiScaler/ext/ffxiv/chara/CharaFlood.cpp', '{repo}/OptiScaler/ext/ffxiv/chara/CharaIdentify.cpp',
                    '{repo}/OptiScaler/ext/core/ExtConfig.cpp', '{repo}/OptiScaler/ext/hdr/HdrConfig.cpp',
                    '{repo}/OptiScaler/ext/hudless/HudlessConfig.cpp', '{repo}/OptiScaler/ext/hudless/HudlessState.cpp')
    IncludeDirs = @('stubs', '{repo}/tests/ext/hdr/stubs', '{repo}/OptiScaler', '{repo}/external/simpleini', '{out}')
    Libs        = @('d3d11.lib', 'dxgi.lib', 'd3dcompiler.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
