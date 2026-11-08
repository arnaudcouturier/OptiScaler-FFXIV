@{
    Class       = 'warp'
    Note        = 'Ambient occlusion pass on D3D11 WARP with the real shaders and the real Pass class on synthetic buffers: a floor meeting a wall is darker where they meet than the open floor beside it, a flat plane stays lit everywhere, a strength of 0 leaves the engine''s own texel at every blend and so does a blend of 0, only the red channel is ever written (the engine''s green, blue and alpha come back untouched at every texel), the whole target matches a CPU reference of the same horizon search within a tolerance and in the mean, a depth allocation twice the occlusion texture''s leaves every texel outside the quadrant the engine''s own producer fills byte for byte the engine''s, the quadrant itself is identical to the same scene rendered as its own allocation, and that quadrant matches the CPU reference at the same 2:1 scale texel for texel (a row of walls, so a texel of ours standing for the block''s second depth texel instead of its first moves the picture and fails), a wall foot at depth texel 40, 40 darkens occlusion texel 20, 20, a cleared depth stays open, two runs of the same scene are byte for byte identical and reuse the two owned textures while a size change creates one pair, the timing pair is begun by every run that can time itself and ended only by the run that began it, a run the rules refuse returns null so the caller keeps the engine''s texture (its occlusion resource is set, so the run reaches the rules and not only the null-argument guard), the darkest texel matches a CPU reference that subtracts the tangent plane''s bias, a frame whose every stored normal faces away from the viewer comes out byte for byte the same corner (the search turns each one back), and the two search bounds a fixture cannot isolate (the far-plane depth guard and the thickness bound) are pinned at their statements'
    Prepare     = 'prepare_ao.ps1'
    Sources     = @('ao_warp.cpp', '{repo}/OptiScaler/ext/ffxiv/engine/AoPass.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler', '{out}')
    Libs        = @('d3d11.lib', 'dxgi.lib')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
