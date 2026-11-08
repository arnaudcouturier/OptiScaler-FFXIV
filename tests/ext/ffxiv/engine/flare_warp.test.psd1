@{
    Class       = 'warp'
    Note        = 'Lens flare on D3D11 WARP: the ghost-step entry (the capture''s own 16-byte cRadialBlurParam, the sun''s own centre and decay as bits) scales the step through the primitive and keeps the centre and the decay bit for bit as the game wrote them, the game''s own buffer is never written, an identity entry the rules would never emit is a byte-exact copy, and the ring''s entry scales the colour row of a 64-byte cHaloParam with a denormal and a NaN payload beside it alive'
    Prepare     = 'prepare_cbpatch.ps1'
    Sources     = @('flare_warp.cpp', '{repo}/OptiScaler/ext/ffxiv/engine/CbPatch.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler', '{out}')
    Libs        = @('d3d11.lib', 'dxgi.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
