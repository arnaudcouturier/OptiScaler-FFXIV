@{
    Class       = 'warp'
    Note        = 'Screen-space light bounce outlier control and edge-aware filter on D3D11 WARP, the real PS_Clamp and PS_Atrous0..3 compiled from Gi.hlsl by prepare_gi.ps1 (the pass''s fxc recipe) and drawn over irradiance and guide textures the test builds, against independent CPU references with their numbers typed in the test: on a flat guide at step 1 the filter is the dense 5x5 binomial stencil (a checker input must come out as one flat 0.5, a random field must come out correlated with no checkerboard component and the independent reference at every texel, a flat field must stay flat at every texel and every step, texels without data contribute nothing and have no answer of their own, an impulse on each border line must read the SKIPPED answer, not the clamped one, a traced texel with no hit is averaged in as the zero it is while an untraceable one is skipped, a lone hit among traced misses is shared over its 25 taps), the later iterations (taps 2, 4 and 8 texels apart) equal the reference, a depth edge is kept (a flat guide mixes it), a slanted plane is one surface to its far end (the inverse depth is linear in the texel position and the slope measured around the texel predicts it), a thin object does not leak into the plane beside it (the slope is the smaller one-sided step), and the clamp holds a texel to 16 times the mean luminance of its other traced neighbours plus 0.1 (a 50x outlier is capped and keeps its hue, the texel is not in its own mean, a lone hit under the floor survives, untraceable neighbours are never read, an isolated traced texel is kept, borders skip taps outside the extent)'
    Prepare     = 'prepare_gi.ps1'
    Sources     = @('gi_filter_warp.cpp')
    IncludeDirs = @('{out}')
    Libs        = @('d3d11.lib', 'dxgi.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
