@{
    Class       = 'warp'
    Note        = 'ISceneHdr::WouldSupplyScene (FfxivSceneHdr::WouldSupply) agrees with Open(presentation) over the cases and touches no counter or scene state, on D3D11/D3D12 WARP'
    Prepare     = 'prepare_scene_shaders.ps1'
    Sources     = @('scene_would_supply.cpp')
    IncludeDirs = @('scene_stubs', 'stubs', '{out}', '{repo}/OptiScaler', '{repo}/external/simpleini')
    Libs        = @('d3d11.lib', 'd3d12.lib', 'dxgi.lib', 'd3dcompiler.lib', 'ole32.lib')
    Defines     = @('NOMINMAX')
}
