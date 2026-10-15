@{
    Class       = 'warp'
    Note        = 'ext::gpu::GpuLifetime retirement, replay, generations, wrapped identity, threads (WARP, standalone)'
    Sources     = @('gpu_lifetime_smoke.cpp', '{repo}/OptiScaler/ext/gpu/GpuLifetime.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Libs        = @('d3d12.lib', 'dxgi.lib', 'ole32.lib')
    Defines     = @('NOMINMAX')
}
