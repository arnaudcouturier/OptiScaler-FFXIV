@{
    Class       = 'warp'
    Note        = 'Ambient occlusion game-object work (ext/ffxiv/engine/AoSwap.cpp and the pure slot rule in AoRules.h) on D3D11 WARP: the game''s own captured reader bytecode parses to the TEXTURE registers for the depth, the normal and the camera (their samplers, reflected first under the same names, are not slots this module reads), and the word stamped on a shader object made from that bytecode names them; the slot word stamped on one pixel shader is read back unchanged after five thousand more shaders are created and released, a shader that was never stamped reads the empty word, a word of 0 is not written, and a null object is no stamp at all; a texture bound at the occlusion slot is swapped for this module''s view and the game''s own view is bound again when the guard goes out of scope, with the object''s reference count unchanged; an exception through the guard restores the same way; an empty slot and a null replacement are no swap at all, the slot is left alone and the guard reports that it has nothing to restore'
    Sources     = @('ao_apply_warp.cpp', '{repo}/OptiScaler/ext/ffxiv/engine/AoSwap.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Libs        = @('d3d11.lib', 'dxgi.lib', 'd3dcompiler.lib')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
