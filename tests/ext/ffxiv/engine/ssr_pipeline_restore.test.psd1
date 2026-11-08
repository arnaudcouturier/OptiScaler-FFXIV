@{
    Class       = 'warp'
    Note        = 'Runs the real SSR pass on the existing WARP harness with distinctive game VS/PS/GS, IA buffers/layout/topology, PS constant buffer/SRV/sampler, scissor rasterizer, blend factor/mask, depth/stencil/ref, viewport/scissor and RTV/DSV bound; D3D getters compare restored object identity and values after Pass::Run.'
    Sources     = @('ssr_pipeline_restore.cpp',
                    '{repo}/OptiScaler/ext/ffxiv/engine/SsrPass.cpp',
                    '{repo}/OptiScaler/ext/ffxiv/engine/SsrApply.cpp',
                    '{repo}/OptiScaler/ext/ffxiv/engine/SsrSwap.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler', '{out}')
    Args        = @('{repo}')
    Libs        = @('d3d11.lib', 'dxgi.lib', 'd3dcompiler.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
    Prepare     = 'prepare_ssr.ps1'
}
