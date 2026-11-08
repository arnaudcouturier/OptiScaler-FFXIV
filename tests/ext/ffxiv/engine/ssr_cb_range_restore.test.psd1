@{
    Class       = 'warp'
    Note        = 'Runs the actual SSR PipelineOnly stage with D3D11.1 PS/VS constant-buffer subranges seeded at slots PS0/2 and VS0/1. A native fullscreen GPU draw reads PS b0 from firstConstant 16 before and after and checks exact output color plus GetConstantBuffers1 buffer/range metadata. Unsupported offsetting is reported as unsupported, not as a preservation failure.'
    Sources     = @('ssr_cb_range_restore.cpp',
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
