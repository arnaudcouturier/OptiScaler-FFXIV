@{
    Class       = 'warp'
    Note        = 'Production D3D12 HDR10 converter, 10-bit PQ output and 203-nit paper white on WARP; scene and mask coverage, barrier counts, legacy and continuous goldens'
    Prepare     = 'prepare_hdr10_shader.ps1'
    Sources     = @('hdr10_converter.cpp', '{repo}/OptiScaler/ext/hdr/Hdr10.cpp')
    IncludeDirs = @('hdr10_stubs', 'stubs', '{out}', '{repo}/OptiScaler', '{repo}/external/simpleini')
    Libs        = @('d3d12.lib', 'dxgi.lib', 'd3dcompiler.lib', 'ole32.lib', 'user32.lib')
    Defines     = @('NOMINMAX')
}
