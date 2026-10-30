@{
    Class       = 'warp'
    Note        = 'HDR census: HUD draw census of the D1 diagnostics (ui-census.txt) on real WARP draws: exclusive classes, boundary restart, other targets, present draws, self-expiry, exact text'
    Sources     = @('ui_census.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Libs        = @('d3d11.lib', 'dxgi.lib', 'd3dcompiler.lib')
    Defines     = @('NOMINMAX')
}
