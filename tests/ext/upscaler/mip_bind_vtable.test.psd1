@{
    Class       = 'cpu'
    Note        = 'Bind-time mip bias vtable slots (ext/upscaler/MipBindEngine.h) pinned against d3d11.h: CreateSamplerState on the device, the six SetSamplers and six GetSamplers on the context'
    Sources     = @('mip_bind_vtable_test.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX', 'UNDEBUG')
    Std         = 'c++latest'
}
