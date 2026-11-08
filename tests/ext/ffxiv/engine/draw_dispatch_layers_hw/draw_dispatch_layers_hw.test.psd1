@{
    Class       = 'local'
    Gpu         = $true
    Note        = 'RTX 4090 FL11_1 native D3D11 Detours test of production Forward/fresh-entry/layer policy around the production SSR pass, in both layer attachment orders, with cached-dispatch negative controls.'
    Sources     = @('draw_dispatch_layers_hw.cpp')
    IncludeDirs = @('{repo}/tests/ext/ffxiv/engine', '{repo}/tests/ext/ffxiv/engine/stubs', '{repo}/OptiScaler', '{repo}/OptiScaler/include', '{out}')
    Defines     = @('NOMINMAX', 'UNDEBUG')
    Std         = 'c++latest'
    Runner      = 'run_draw_dispatch_layers_hw.ps1'
}
