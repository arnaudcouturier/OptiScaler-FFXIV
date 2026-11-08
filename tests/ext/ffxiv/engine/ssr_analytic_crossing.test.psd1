@{
    Class       = 'warp'
    Note        = 'Analytic floor-to-wall physical intersection sweep through the SSR WARP pass'
    Sources     = @('ssr_analytic_crossing.cpp',
                    '{repo}/OptiScaler/ext/ffxiv/engine/SsrApply.cpp',
                    '{repo}/OptiScaler/ext/ffxiv/engine/SsrPass.cpp',
                    '{repo}/OptiScaler/ext/ffxiv/engine/SsrSwap.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler', '{out}')
    Args        = @('{repo}')
    Libs        = @('d3d11.lib', 'dxgi.lib', 'd3dcompiler.lib', 'user32.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
    Prepare     = 'prepare_ssr.ps1'
}
