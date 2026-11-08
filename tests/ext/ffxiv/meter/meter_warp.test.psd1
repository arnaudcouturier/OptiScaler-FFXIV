@{
    Class       = 'warp'
    Note        = 'Effect meter pass (MeterPass.cpp + Meter.hlsl) on D3D11 WARP with the real shader and the real Pass class on synthetic 1080p pictures: a known change gives the known share, mean and coverage (the chain stage in display terms against the typed white, the occlusion on its red channel and its occluded texels, the sun mask on its lit texels, the planar map on its reflecting texels), the GPU totals equal the CPU tap emulation (same geometry, hash and seed) and land within 1 % of the CPU full diff, key off (output equals input) reads exactly 0, a window crop measures the dump''s box only, a NaN texel counts as nothing, the game''s state (t0/t1, b0, render target, viewport, shaders, topology) comes back after a start, a start while one is in flight and every view the pass cannot read (SRGB, multisampled, array, unequal sizes, a window outside the texture) fail with a reason and queue nothing, one draw per measurement'
    Prepare     = 'prepare_meter.ps1'
    Sources     = @('meter_warp.cpp', '{repo}/OptiScaler/ext/ffxiv/meter/MeterPass.cpp')
    IncludeDirs = @('../engine/stubs', '../engine', '{repo}/OptiScaler', '{out}')
    Libs        = @('d3d11.lib', 'dxgi.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
