@{
    Class       = 'warp'
    Note        = 'Bind-time mip bias sampler table and variants (ext/upscaler/MipBindEngine.h): lock-free insert/find/refresh/invalidate/replace/full, threads, and variants made on a real D3D11 WARP device in add and replace'
    Sources     = @('mip_bind_table_test.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Libs        = @('d3d11.lib', 'dxgi.lib')
    Defines     = @('NOMINMAX', 'UNDEBUG')
    Std         = 'c++latest'
}
