@{
    Class       = 'warp'
    Note        = 'S10 UI paste on WARP through the public API (Produce -> Paste -> backbuffer readback): threshold, dilation 0..5, cleanup levels, SDR + PQ storage, coverage and amplitude counters, CPU reference on random images, ring / fence / queue rules, a Release while a produce pass that reads the previous image is still queued (D3D12 debug layer live-object report), steady timing, stale, resize, Release'
    Prepare     = 'prepare_ui_shaders.ps1'
    Sources     = @('ui_paste_warp.cpp', '{repo}/OptiScaler/ext/hudless/UiPaste.cpp', '{repo}/OptiScaler/ext/gpu/Gpu.cpp', '{repo}/OptiScaler/ext/gpu/GpuLifetime.cpp')
    IncludeDirs = @('ui_stubs', '../gpu/stubs', '{out}', '{repo}/OptiScaler')
    Libs        = @('d3d12.lib', 'dxgi.lib', 'ole32.lib')
    Defines     = @('NOMINMAX')
}
