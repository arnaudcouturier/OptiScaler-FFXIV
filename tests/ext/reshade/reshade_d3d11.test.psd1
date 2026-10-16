@{
    Class       = 'warp'
    Note        = 'S10 D0: D3D11 helpers on WARP: SEH-guarded call, format families, pipeline state block round trip, sampled before/after target comparison'
    Sources     = @('reshade_d3d11.cpp', '{repo}/OptiScaler/ext/reshade/ReShadeD3D11.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Libs        = @('d3d11.lib', 'dxgi.lib')
    Defines     = @('NOMINMAX', 'UNDEBUG')
    Std         = 'c++latest'
}
