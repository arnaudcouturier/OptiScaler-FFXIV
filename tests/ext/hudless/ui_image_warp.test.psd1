@{
    Class       = 'warp'
    Note        = 'S10 recomposition mask on WARP: real UiImage.cpp + UiRecomposition.hlsl (threshold edges, dilation 0..5, image-edge clamping, 5120x2160 and 5119x2158, alpha strictly 0/1, slot/lifetime handling)'
    Prepare     = 'prepare_ui_shaders.ps1'
    Sources     = @('ui_image_warp.cpp', '{repo}/OptiScaler/ext/hudless/UiImage.cpp', '{repo}/OptiScaler/ext/gpu/Gpu.cpp', '{repo}/OptiScaler/ext/gpu/GpuLifetime.cpp')
    IncludeDirs = @('ui_stubs', '../gpu/stubs', '{out}', '{repo}/OptiScaler')
    Libs        = @('d3d12.lib', 'dxgi.lib', 'ole32.lib')
    Defines     = @('NOMINMAX')
}
