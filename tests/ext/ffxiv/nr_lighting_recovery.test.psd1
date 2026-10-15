@{
    Class       = 'warp'
    Note        = 'Native lighting readback generation, stale sample, disable, and GPU error recovery through D3D11 WARP'
    Sources     = @('nr_lighting_recovery.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Libs        = @('d3d11.lib', 'dxgi.lib', 'ole32.lib')
    Defines     = @('NOMINMAX')
}
