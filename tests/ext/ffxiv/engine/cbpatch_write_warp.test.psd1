@{
    Class       = 'warp'
    Note        = 'Engine pass override CPU-written mode on D3D11 WARP: the bound slot carries this module''s buffer for the scope and the game''s buffer is back afterwards (also when the draw throws), a buffer that has never been written is written in frame 0 rather than handed back unwritten, a frame writes its values once however many draws read them, a frame whose target equals the engine''s value swaps nothing, an impossible request hands back null, a guard with no replacement binds nothing, and the values the draw reads are the caller''s byte for byte'
    Prepare     = 'prepare_cbpatch.ps1'
    Sources     = @('cbpatch_write_warp.cpp', '{repo}/OptiScaler/ext/ffxiv/engine/CbPatch.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler', '{out}')
    Libs        = @('d3d11.lib', 'dxgi.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
