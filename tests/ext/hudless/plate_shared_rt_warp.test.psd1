@{
    Class       = 'warp'
    Note        = 'Render-target plate slot: a D3D11 render-target-bindable NT-shared texture is cleared and drawn through its render target view with the game depth/blend state, read back through D3D12, sampled by a D3D12 draw, the default copy-target slot is unchanged, bad input refused'
    Sources     = @('plate_shared_rt_warp.cpp', '{repo}/OptiScaler/ext/hudless/SharedSlot.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Libs        = @('d3d11.lib', 'd3d12.lib', 'dxgi.lib', 'd3dcompiler.lib', 'ole32.lib')
    Defines     = @('UNDEBUG', 'NOMINMAX')
}
