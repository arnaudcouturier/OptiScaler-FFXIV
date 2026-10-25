@{
    Class       = 'warp'
    Note        = 'Automatic ReShade G-buffers, D3D11 half on WARP: format table (full sweep), request checks, constants and motion multiplier, the render-window remap of every supported format family against two CPU references (also 8888 sRGB twins, scaled motion vectors), cache, target and view lifetime, the game context state (also with the source bound in it), bypass scope, refusals, faults, a stuck swap back, DescribeView and the engine routing, the reported motion scale, the negative target cache, the process-wide context-state mutex (two threads, depth pass and G-buffer pass), the product shader driven directly (clamp, largest edge)'
    Prepare     = 'prepare_gbuffer_copy.ps1'
    Sources     = @('reshade_gbuffer_copy.cpp', '{repo}/OptiScaler/ext/reshade/ReShadeGBufferCopy.cpp', '{repo}/OptiScaler/ext/reshade/ReShadeDepthCopy.cpp', '{repo}/OptiScaler/ext/reshade/ReShadeD3D11.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler', '{out}/ext_gen')
    Libs        = @('d3d11.lib', 'dxgi.lib')
    Defines     = @('NOMINMAX', 'UNDEBUG', 'DEPTH_TEST_LOG')
    Std         = 'c++latest'
}
