@{
    Class       = 'warp'
    Note        = 'Bind-time mip bias through REAL Detours hooks on a D3D11 WARP device and its immediate context (ext/upscaler/MipBindEngine.h, the same install as MipBindHooks.cpp): CreateSamplerState classification, the six SetSamplers / GetSamplers detours reached through the ordinary D3D11 calls, a second hook layered on the same function (as upstream''s create hook is), and detach'
    Sources     = @('mip_bind_detours_test.cpp', '{repo}/OptiScaler/library/detours/detours.lib')
    IncludeDirs = @('{repo}/OptiScaler', '{repo}/OptiScaler/include')
    Libs        = @('d3d11.lib', 'dxgi.lib')
    Defines     = @('NOMINMAX', 'UNDEBUG')
    Std         = 'c++latest'
}
