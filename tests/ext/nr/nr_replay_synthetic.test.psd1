@{
    Class       = 'warp'
    Note        = 'NR frame replay end to end on D3D12 WARP with a set the test writes: twice-run determinism per dispatch kind, typeless and depth-plane views, no-reference failure, bless, compare, changed constant or shader, partial bless, adapter change, damaged image'
    Prepare     = 'prepare_shaders.ps1'
    Sources     = @('nr_replay_synthetic.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Libs        = @('d3d12.lib', 'dxgi.lib', 'd3dcompiler.lib', 'version.lib')
    Defines     = @('NOMINMAX')
    Args        = @('{out}/shaders/dlssnr.hlsl')
}
