@{
    Class       = 'warp'
    Note        = 'Production DLSS HDR scene adapter, scene fallback, reconstructed output, PreSR reconciliation and typed FP16 input on D3D12 WARP'
    Prepare     = 'prepare_scene_input_shaders.ps1'
    Sources     = @('dlss_scene_dx12.cpp')
    IncludeDirs = @('hdr10_stubs', 'input_stubs', '{out}', '{repo}/OptiScaler')
    Libs        = @('d3d12.lib', 'dxgi.lib', 'd3dcompiler.lib', 'ole32.lib')
    Defines     = @('NOMINMAX')
}
