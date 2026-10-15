@{
    Class       = 'warp'
    Note        = 'PreSR padded colour: active-extent detection and copy (D3D12 WARP; DlssNr_ActiveColor.h)'
    Sources     = @('nr_active_color_smoke.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Libs        = @('d3d12.lib', 'dxgi.lib')
    Defines     = @('NOMINMAX')
}
