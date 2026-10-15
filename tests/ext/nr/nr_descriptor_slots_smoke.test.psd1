@{
    Class       = 'warp'
    Note        = 'NR DescriptorSlots<N>: frame budget, no ring wrap, replayable recordings, abandoned submissions (WARP; the NR half of the former gpu_lifetime_smoke)'
    Sources     = @('nr_descriptor_slots_smoke.cpp', '{repo}/OptiScaler/ext/gpu/GpuLifetime.cpp')
    IncludeDirs = @('{repo}/tests/ext/gpu/stubs', '{repo}/OptiScaler')
    Libs        = @('d3d12.lib', 'dxgi.lib', 'ole32.lib')
    Defines     = @('NOMINMAX')
}
