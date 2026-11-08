@{
    Class       = 'warp'
    Note        = 'Screen-space light bounce irradiance smooth on D3D11 WARP, the real PS_Smooth compiled from Gi.hlsl by prepare_gi.ps1 (the pass''s fxc recipe) and drawn over textures the test builds: a checker input (even x+y = 1, odd = 0, data everywhere) must come out as one flat 0.5 (the parity classes mix; the old stencil kept them at 1 and 0, a contrast of 100%, which is the diamond lattice of the first game run), a seeded random field must come out with correlated neighbours, no checkerboard component, at least the old ring''s noise reduction and the independent binomial reference at every texel, a flat field must stay flat at every texel including borders and corners, texels without data (alpha 0 or NaN behind huge or NaN colour) contribute nothing and a texel with no data in its whole reach comes out (0,0,0,0), an impulse on each border line must read the SKIPPED answer, not the clamped one (hand-computed values), and a traced texel with no hit (alpha above 0, rgb 0) is averaged in as the zero it is while an untraceable one (alpha 0) is skipped (a lone hit among traced misses is shared over its 25 taps: 36/256 at its own texel; hand-computed rows next to an untraceable region)'
    Prepare     = 'prepare_gi.ps1'
    Sources     = @('gi_smooth_warp.cpp')
    IncludeDirs = @('{out}')
    Libs        = @('d3d11.lib', 'dxgi.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
