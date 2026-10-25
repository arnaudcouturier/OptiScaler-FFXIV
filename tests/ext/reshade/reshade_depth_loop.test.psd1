@{
    Class       = 'warp'
    Note        = 'Automatic ReShade depth, the loop of a session: the real CaptureEngine (WARP, fake parameter block and host), the real binder over the fake ReShade world and the real event dispatcher agree on the frame stamp (delta 0), the clock, the native device and the consumer size, a full cycle ends in Bound and again the next frame, a preview-sized Output or another back buffer size or device never binds, finish ticks the counter also for refused and missing captures so a capture that stops goes stale'
    Prepare     = 'prepare_depth_copy.ps1'
    Sources     = @('reshade_depth_loop.cpp', '{repo}/OptiScaler/ext/reshade/ReShadeDepthEval.cpp', '{repo}/OptiScaler/ext/reshade/ReShadeDepthCopy.cpp', '{repo}/OptiScaler/ext/reshade/ReShadeD3D11.cpp', '{repo}/OptiScaler/ext/reshade/ReShadeDepthCore.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler', '{repo}/external/nvngx_dlss_sdk', '{out}/ext_gen')
    Libs        = @('d3d11.lib', 'dxgi.lib')
    Defines     = @('NOMINMAX', 'UNDEBUG', 'DEPTH_TEST_LOG')
    Std         = 'c++latest'
}
