@{
    Class       = 'warp'
    Note        = 'Runs the same pipeline-only SSR constant-buffer range probe as ssr_cb_range_restore on the machine hardware D3D11 adapter (FL11_0). The test logs adapter/vendor/feature level, checks offsetting support, PS0 range 16/16 and exact green GPU output before and after the real pass, plus PS2/VS0/VS1 metadata preservation; this recipe class runs in focused filters; the executable explicitly selects hardware, never WARP.'
    Sources     = @('ssr_cb_range_restore.cpp',
                    '{repo}/OptiScaler/ext/ffxiv/engine/SsrPass.cpp',
                    '{repo}/OptiScaler/ext/ffxiv/engine/SsrApply.cpp',
                    '{repo}/OptiScaler/ext/ffxiv/engine/SsrSwap.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler', '{out}')
    Args        = @('{repo}', '--hardware')
    Libs        = @('d3d11.lib', 'dxgi.lib', 'd3dcompiler.lib', 'user32.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
    Prepare     = 'prepare_ssr.ps1'
}
