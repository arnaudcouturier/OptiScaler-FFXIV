@{
    Class       = 'warp'
    Note        = 'God rays (beams that follow the engine''s own shadows) on D3D11 WARP with the real shaders and the real Pass class on synthetic buffers, capture-shaped projection (reversed Z, no far plane, w = 0 on sky): the term matches a CPU reference march per pixel (all four cascades each inside its own atlas slice read from its matrix, the writer''s tail, the Henyey-Greenstein phase, the in-scatter integral with extinction, the 0.002 air bias, softness 0/1/2, jitter on and off), each rule is seen by a fixture that separates it from its broken form (first cascade wins, no slice test, no bias, no extinction, the cos^16 lobe), a strip occluder gives the rings around the light a structure >= 0.3 and >= 0.3 above the all-lit control, the composite equals a CPU composite (bilateral upsample keeping the wall/sky edge, the ShaftAdded bound, the sky ceiling), the sky gets light but never crosses its own 99th percentile (5 % crosses without the ceiling, no sky samples leave it alone), the stats pass reads back ceilings, sample count and term peak, the game''s graphics and compute state come back after a run, the fog read without the height profile returns the sun row, the stolen views, and a binding the pass cannot read returns null with a reason'
    Prepare     = 'prepare_godrays.ps1'
    Sources     = @('godrays_warp.cpp', '{repo}/OptiScaler/ext/ffxiv/engine/GodraysPass.cpp',
                    '{repo}/OptiScaler/ext/ffxiv/engine/GodraysSun.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler', '{out}')
    Libs        = @('d3d11.lib', 'dxgi.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
