@{
    Class       = 'warp'
    Note        = 'Bind-time mip bias control logic (ext/upscaler/MipBindControl.h) on real D3D11 WARP samplers and contexts: the whole evaluate flow with the real FFXIV numbers in add and replace, a hand-set value blocking and then releasing, duplicate creates, alternating features, the full table, the context relatch, the latch watch and the install plan'
    Sources     = @('mip_bind_control_test.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Libs        = @('d3d11.lib', 'dxgi.lib')
    Defines     = @('NOMINMAX', 'UNDEBUG')
    Std         = 'c++latest'
}
