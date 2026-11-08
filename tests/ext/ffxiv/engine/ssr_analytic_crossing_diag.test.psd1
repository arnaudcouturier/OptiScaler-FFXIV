@{
    Class = 'warp'
    Note = 'Original shader exit reasons for analytically visible wall reflections on coherent floor-wall scenes.'
    Sources = @('ssr_analytic_crossing.cpp',
        '{repo}/OptiScaler/ext/ffxiv/engine/SsrApply.cpp',
        '{repo}/OptiScaler/ext/ffxiv/engine/SsrPass.cpp',
        '{repo}/OptiScaler/ext/ffxiv/engine/SsrSwap.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler', '{out}')
    Args = @('{repo}')
    Libs = @('d3d11.lib', 'dxgi.lib', 'd3dcompiler.lib')
    Defines = @('NOMINMAX', 'SSR_ANALYTIC_DIAG')
    Std = 'c++latest'
    Prepare = 'prepare_ssr_raw_diag.ps1'
}
