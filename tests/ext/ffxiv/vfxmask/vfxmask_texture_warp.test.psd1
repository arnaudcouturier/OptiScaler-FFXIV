@{
    Class       = 'warp'
    Note        = 'VFX mask gate probe texture on the WARP adapter: the production code builds the R8_UNORM mask of the colour input''s size and the readback shows the masked half at 1.0 for left and for right (even, odd and 3840x2160), re-use per size and mode, the refusals (no context, off, one column, empty height, edge too large) and the description guard checked before every write'
    Sources     = @('vfxmask_texture_warp.cpp', '{repo}/OptiScaler/ext/ffxiv/vfxmask/VfxMaskTexture.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Libs        = @('d3d11.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
