@{
    Class       = 'warp'
    Note        = 'Engine pass override GPU set on D3D11 WARP: the wetness entry replaces .x and .y of a 32-byte buffer with the caller''s exact bits and every other byte - a NaN payload and a denormal in .zw, the smallest normal, a NaN filling every bit, negative zero and an infinity in the second float4 - comes out bit for bit as it went in (the whole buffer is compared with memcmp, not the addressed words), the dryness blend is 1 - w/2, an entry that addresses the second float4 only leaves the first alone, a zero mask is a plain copy, the game''s own buffer is never written, a request that cannot be honoured (entry past the end, no entry, no buffer, wrong size) returns null with a reason and dispatches nothing, and a release leaves nothing behind'
    Prepare     = 'prepare_cbpatch.ps1'
    Sources     = @('cbpatch_set_warp.cpp', '{repo}/OptiScaler/ext/ffxiv/engine/CbPatch.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler', '{out}')
    Libs        = @('d3d11.lib', 'dxgi.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
