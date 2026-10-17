@{
    Class       = 'warp'
    Note        = 'S10-E2: live anchor R1b end to end on WARP (draw-time core + live::EventOf + live source + fake effects): a preview-RT UI draw first and the backbuffer UI draw second -> the backbuffer draw is the anchor and the frame is captured; only preview-RT UI draws -> NotThisFrame, no rejection; HUD first on the backbuffer unchanged; supersession; unusable backbuffer format rejected without side effects'
    Sources     = @('ui_live_anchor_warp.cpp', '{repo}/OptiScaler/ext/reshade/ReShadeBoundaryCore.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Libs        = @('d3d11.lib', 'dxgi.lib', 'd3dcompiler.lib')
    Defines     = @('NOMINMAX')
}
