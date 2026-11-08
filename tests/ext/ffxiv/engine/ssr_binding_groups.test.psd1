@{
    Class       = 'warp'
    Note        = 'Runs production SSR reader controls O/P and pins setter counts, no-preparation behavior, and restored native SRV/RTV bindings.'
    Sources     = @('ssr_binding_groups.cpp',
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
