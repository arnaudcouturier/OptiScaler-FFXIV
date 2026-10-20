@{
    Class       = 'warp'
    Note        = 'Bind-time mip bias cost of one SetSamplers call (ext/upscaler/MipBindEngine.h) with a no-op native: pass-through, record hits, record misses with cached variants; prints ns per call and per slot'
    Sources     = @('mip_bind_cost_test.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Libs        = @('d3d11.lib', 'dxgi.lib')
    Defines     = @('NOMINMAX', 'UNDEBUG')
    Std         = 'c++latest'
}
