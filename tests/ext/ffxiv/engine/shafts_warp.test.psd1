@{
    Class       = 'warp'
    Note        = 'Light shaft pass on D3D11 WARP with the real shaders and the real Pass class on synthetic buffers: the sky mask is 1 exactly where the depth is the far plane (the right half of the frame) and 0 on the wall, the composite brightens the wall next to the sky and never darkens anything, the increase falls off with the distance from the sun, the decay key is the weighted mean the blur itself states (the read-back matches that formula at two decays, and the two decays give different terms, which a weight that never falls off cannot), the far side of the frame is unchanged, the sky keeps its own value byte for byte, intensity 0 is a byte-exact copy of the scene, a half-pixel viewport origin (the engine''s own 1920x1080+0.5+0.5) still copies every scene texel to its own pixel, the sun''s colour comes from the sky around it and not from the dark leaf in front of the sun, and a frame with no sky near the sun adds nothing, the live R24_UNORM_X8_TYPELESS depth is read through the same path as R32_FLOAT, a second run reuses the scene copy and the half-resolution pair, a dynamic-resolution window re-creates the half pair but not the scene copy, and a binding the pass cannot read (an 8-bit scene, a depth of another format, an empty window) returns null with a reason instead of a partial frame; and the fixture''s white point is the captured frame''s measured 0.838, not the pass''s fallback'
    Prepare     = 'prepare_shafts.ps1'
    Sources     = @('shafts_warp.cpp', '{repo}/OptiScaler/ext/ffxiv/engine/ShaftsPass.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler', '{out}')
    Libs        = @('d3d11.lib', 'dxgi.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
