@{
    Class       = 'warp'
    Note        = 'Production NR HDR scene adapter, viewport crop, identity/edit round trips, alpha/padding and GPU recording retirement on D3D12 WARP'
    Prepare     = 'prepare_scene_input_shaders.ps1'
    Sources     = @('nr_scene_input.cpp', '{repo}/OptiScaler/ext/gpu/GpuLifetime.cpp')
    IncludeDirs = @('input_stubs', '{out}', '{repo}/tests/ext/gpu/stubs', '{repo}/OptiScaler')
    Libs        = @('d3d12.lib', 'dxgi.lib', 'd3dcompiler.lib', 'ole32.lib')
    Defines     = @('NOMINMAX')
}
