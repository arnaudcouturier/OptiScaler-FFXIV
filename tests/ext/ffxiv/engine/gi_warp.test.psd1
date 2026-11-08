@{
    Class       = 'warp'
    Note        = 'Screen-space light bounce on D3D11 WARP with the real shaders and the real Pass class on synthetic buffers: a 128x96 fixture of one floor plane (its rays'' sky overhead) with one dim slab hanging over a stretch of it, the floor''s scene texels written as the albedo store times the light''s square root on a checker of two stores (so the floor''s own linear value is 2.56 on one texel and 0.36 on the next), the pass''s irradiance matched against a CPU reference march that reads each hit texel''s own linear scene value (its stored value squared; never the albedo target, never a quotient) and counts the rays it marched (the ramp, crossing and thickness re-derived from the rules), the store''s alpha matched against that traced count at every pass texel (the open floor''s all-miss texels are traced zeros, the sky and a block of cleared-normal floor carry none), a lamp band (stored 2.5, bounded by the clamp) and an over-the-clamp band (stored 3.2, refused) run through the same pass, the coverage''s two classes (the covered floor''s estimate carries the dim slab, the open floor''s estimate is nothing at all), the smooth matched against an independent CPU reference of the dense 5x5 binomial stencil (weights typed in the test, not read from the rules header; data marker = the reference''s traced count; the parity itself is decided by gi_smooth_warp), the apply''s bounded addition (the sky exact, no channel past the white, the covered floor''s adds above the open floor''s zeros, the strength key''s ratio linear until the bound descales it), the pass''s null-with-reason on a binding it cannot read, and its timing pair''s per-run begins and ends'
    Prepare     = 'prepare_gi.ps1'
    Sources     = @('gi_warp.cpp', '{repo}/OptiScaler/ext/ffxiv/engine/GiPass.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler', '{out}')
    Libs        = @('d3d11.lib', 'dxgi.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
