@{
    Class       = 'warp'
    Note        = 'ext::gpu::GpuLifetime retirement-sequence gate (WARP): monotonic mark, pending hold, later retirements never hold an earlier mark'
    Sources     = @('gpu_lifetime_seq.cpp', '{repo}/OptiScaler/ext/gpu/GpuLifetime.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Libs        = @('d3d12.lib', 'dxgi.lib', 'ole32.lib')
    Defines     = @('NOMINMAX')
}
