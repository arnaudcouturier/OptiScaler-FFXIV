@{
    Class       = 'warp'
    Note        = 'NR frame-replay recorder on D3D12 WARP: nothing recorded unless asked, copies shared until a write, typeless and depth-plane resources, set written and read back, and replayed to the very bytes the dispatch produced'
    Sources     = @('nr_replay_capture.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Libs        = @('d3d12.lib', 'dxgi.lib', 'd3dcompiler.lib', 'version.lib')
    Defines     = @('NOMINMAX')
}
