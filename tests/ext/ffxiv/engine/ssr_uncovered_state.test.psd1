@{
    Class       = 'warp'
    Note        = 'Runs Pass::Run in draw and diagnosticSkipDraw modes, checking OM UAV beyond RTV span, CS UAV, predication, and SO binding via getters.'
    Sources     = @('ssr_uncovered_state.cpp',
                    '{repo}/OptiScaler/ext/ffxiv/engine/SsrPass.cpp',
                    '{repo}/OptiScaler/ext/ffxiv/engine/SsrApply.cpp',
                    '{repo}/OptiScaler/ext/ffxiv/engine/SsrSwap.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler', '{out}')
    Args        = @('{repo}')
    Libs        = @('d3d11.lib', 'dxgi.lib', 'd3dcompiler.lib', 'user32.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
    Prepare     = 'prepare_ssr.ps1'
}
