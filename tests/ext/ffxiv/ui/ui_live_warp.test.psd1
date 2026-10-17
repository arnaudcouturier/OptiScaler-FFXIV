@{
    Class       = 'warp'
    Note        = 'S10-E: live boundary capture ORDER on WARP: the fake effects (rewrite the render target) run before the snapshot copy, the later UI draw is not in the snapshot; guard rejections have no side effects, failed effects deliver no frame, engine-asked gate, once per frame, size change'
    Sources     = @('ui_live_warp.cpp', '{repo}/OptiScaler/ext/reshade/ReShadeBoundaryCore.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Libs        = @('d3d11.lib', 'dxgi.lib', 'd3dcompiler.lib')
    Defines     = @('NOMINMAX')
}
