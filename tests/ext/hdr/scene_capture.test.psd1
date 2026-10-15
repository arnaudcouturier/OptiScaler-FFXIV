@{
    Class       = 'warp'
    Note        = 'RenoDX FP16 scene capture, same-frame shared-image import, format/viewport checks and scene lifetime on D3D11/D3D12 WARP'
    Prepare     = 'prepare_scene_shaders.ps1'
    Sources     = @('scene_capture.cpp')
    IncludeDirs = @('scene_stubs', 'stubs', '{out}', '{repo}/OptiScaler', '{repo}/external/simpleini')
    Libs        = @('d3d11.lib', 'd3d12.lib', 'dxgi.lib', 'd3dcompiler.lib', 'ole32.lib')
    Defines     = @('NOMINMAX')
}
