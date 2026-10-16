@{
    Class       = 'warp'
    Note        = 'S10 slot ring shared texture on D3D11/D3D12 WARP: typeless capture into a typed slot, D3D12 readback, shader-resource access, independent slots, bad input'
    Sources     = @('hudless_shared_slot.cpp', '{repo}/OptiScaler/ext/hudless/SharedSlot.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Libs        = @('d3d11.lib', 'd3d12.lib', 'dxgi.lib', 'ole32.lib')
    Defines     = @('UNDEBUG', 'NOMINMAX')
}
