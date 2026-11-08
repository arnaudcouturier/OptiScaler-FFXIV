@{
    Class       = 'warp'
    Note        = 'Engine light controls, constant-buffer family on D3D11 WARP through the real CbPatch: the moon patched at +32 diffuse and +48 specular of a 512-byte g_LightParam (position, direction and every other byte bit for bit, seeded NaN payloads and denormals included, the game''s buffer never written), the water pass''s copy with its own factor, the composite''s SH rows per channel and the header sky scale composed with the shipped knob''s entries in one dispatch, the water block''s eight entries, a reader inside the swap that maps what it fetched reads the engine''s bytes while an unmapped one would read the darkened moon, the mapping nests and ends with its scope (also on a throw), and a frame of the four patched sizes evicts nothing from the primitive''s size cache'
    Prepare     = 'prepare_cbpatch.ps1'
    Sources     = @('enginelight_warp.cpp', '{repo}/OptiScaler/ext/ffxiv/engine/CbPatch.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler', '{out}')
    Libs        = @('d3d11.lib', 'dxgi.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
