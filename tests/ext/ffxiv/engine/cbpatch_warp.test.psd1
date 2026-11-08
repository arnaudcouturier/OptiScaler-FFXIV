@{
    Class       = 'warp'
    Note        = 'Engine pass override GPU scale on D3D11 WARP: the ambient knob''s entry halves the 64 addressed .x values of a 16,384-byte buffer and every byte it does not address comes out bit for bit as it went in (the fixture seeds a denormal, NaN payloads and integer-as-float values beside the knob, and the whole buffer is compared, not just the addressed words), the game''s own buffer is never written, a DYNAMIC source - the usage the engine''s own tables likely have - gives the same bytes, a second run reuses the size''s three buffers, an all-ones entry is a plain copy, a request that cannot be honoured (entry past the end, no entry, no buffer, wrong size) returns null with a reason and dispatches nothing, the declared 12,352-byte size works too, and a release leaves nothing behind'
    Prepare     = 'prepare_cbpatch.ps1'
    Sources     = @('cbpatch_warp.cpp', '{repo}/OptiScaler/ext/ffxiv/engine/CbPatch.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler', '{out}')
    Libs        = @('d3d11.lib', 'dxgi.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
