@{
    Class       = 'warp'
    Note        = 'God rays'' cascade and fog read-backs (GodraysSun.cpp) on D3D11 WARP with buffers the test writes: a cascade slot reused by a later frame holds that frame''s copies (the count of the slot''s earlier frame no longer survives, so the ring still delivers ten and four hundred frames into a session, with the frame number it was written at), a fifth buffer in one frame is still refused, the fog read decodes a live capture''s pair through the profile buffer it was given, refuses the clip-to-world matrix and a damaged profile, reports a frame without a profile and clears an older frame''s flag when the slot is reused, and carries the viewport buffer''s second row raw'
    Sources     = @('recog_ring_warp.cpp', '{repo}/OptiScaler/ext/ffxiv/engine/GodraysSun.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Libs        = @('d3d11.lib', 'dxgi.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
