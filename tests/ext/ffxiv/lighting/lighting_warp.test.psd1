@{
    Class       = 'warp'
    Note        = 'The lighting tab''s chara ambient, the 160-byte block''s GPU scale on D3D11 WARP: at the identity the whole block is byte for bit the game''s, at a lift of 1.5 (and at 0.5, the dimming half) the block''s reciprocal-scale field carries the product and every byte beside it - the colour, the two SH row sets, the attenuation and the fields after them, seeded with denormals, NaN payloads and negative zero - is unaffected; the game''s own buffer is never written; a wrong bound width, an entry past the end and no entry hand back null and dispatch nothing; a second run reuses the size''s three buffers and a release bumps the generation'
    Prepare     = '../engine/prepare_cbpatch.ps1'
    Sources     = @('lighting_warp.cpp', '{repo}/OptiScaler/ext/ffxiv/engine/CbPatch.cpp')
    IncludeDirs = @('stubs', '{out}', '{repo}/OptiScaler')
    Libs        = @('d3d11.lib', 'dxgi.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
