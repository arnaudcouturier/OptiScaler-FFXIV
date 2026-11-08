@{
    Class       = 'warp'
    Note        = 'Puddles game-object work (ext/ffxiv/engine/GlossPuddlesSwap.cpp) on D3D11 WARP: the slot word stamped on one pixel shader is read back unchanged after five thousand more shaders are created and released, a shader that was never stamped reads the empty word, a word of 0 is not written, and a null object is no stamp at all; a texture bound at the G-buffer target''s slot is swapped for this module''s view and the game''s own view is bound again when the guard goes out of scope, with the object''s reference count unchanged; an exception through the guard restores the same way; an empty slot and a null replacement are no swap at all, the slot is left empty (nothing swapped in, no reference taken on this module''s view) and the guard reports that it has nothing to restore'
    Sources     = @('gloss_puddles_apply_warp.cpp', '{repo}/OptiScaler/ext/ffxiv/engine/GlossPuddlesSwap.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Libs        = @('d3d11.lib', 'dxgi.lib', 'd3dcompiler.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
