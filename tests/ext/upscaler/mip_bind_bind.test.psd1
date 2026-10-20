@{
    Class       = 'warp'
    Note        = 'Bind-time mip bias on a real D3D11 WARP context (ext/upscaler/MipBindEngine.h): substitution in the six stages, GetSamplers returning the game''s sampler, null slots and ranges, deferred and foreign contexts, shared variants under replace, a new target, ClearState, switching off'
    Sources     = @('mip_bind_bind_test.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Libs        = @('d3d11.lib', 'dxgi.lib')
    Defines     = @('NOMINMAX', 'UNDEBUG')
    Std         = 'c++latest'
}
