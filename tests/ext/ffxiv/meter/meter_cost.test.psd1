@{
    Class       = 'local'
    Gpu         = $true
    Note        = 'Effect meter GPU cost on this machine''s GPU: the real pass (draw + corner copy, timed by its own disjoint timestamp pair) on FP16 3840x2160 and 1920x1080, an occlusion-sized B8G8R8A8 and a 3840x2160 R8 mask, 200 samples each; the median must be within the 0.05 ms per sampled frame budget. The game must be absent'
    Prepare     = 'prepare_meter.ps1'
    Sources     = @('meter_cost.cpp', '{repo}/OptiScaler/ext/ffxiv/meter/MeterPass.cpp')
    IncludeDirs = @('../engine/stubs', '../engine', '{repo}/OptiScaler', '{out}')
    Libs        = @('d3d11.lib', 'dxgi.lib', 'd3dcompiler.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
