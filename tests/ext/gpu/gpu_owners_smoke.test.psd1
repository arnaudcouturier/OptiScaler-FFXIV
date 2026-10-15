@{
    Class       = 'warp'
    Note        = 'ext::gpu owner registry, Reset/Execute wrappers, batch isolation, throwing owner, Scope (WARP, standalone)'
    Sources     = @('gpu_owners_smoke.cpp', '{repo}/OptiScaler/ext/gpu/Gpu.cpp', '{repo}/OptiScaler/ext/gpu/GpuLifetime.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Libs        = @('d3d12.lib', 'dxgi.lib', 'ole32.lib')
    Defines     = @('NOMINMAX')
}
