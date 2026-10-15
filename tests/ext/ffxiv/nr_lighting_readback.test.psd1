@{
    Class       = 'warp'
    Note        = 'Native lighting readback scheduling, identity, query, and color lineage through D3D11 WARP'
    Sources     = @('nr_lighting_readback.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Libs        = @('d3d11.lib', 'dxgi.lib', 'ole32.lib')
    Defines     = @('NOMINMAX')
}
