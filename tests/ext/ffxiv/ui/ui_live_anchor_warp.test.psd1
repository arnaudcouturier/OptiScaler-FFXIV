@{
    Class       = 'warp'
    Note        = 'S10-E2: live anchor R1b end to end on WARP (draw-time core + live::EventOf + live source + fake effects): a preview-RT UI draw first and the backbuffer UI draw second -> the backbuffer draw is the anchor and the frame is captured; only preview-RT UI draws -> NotThisFrame, no rejection; HUD first on the backbuffer unchanged; supersession; unusable backbuffer format rejected without side effects; AnchorRule = early: an off-screen first UI draw captures the BACKBUFFER there (not the off-screen target, not the later backbuffer draws), one copy, no effects call, Ok; presets that would run or fail keep R1b; skip; a scene stage before R1b takes the early capture back and R1b acts (re-armed, also with the copy skipped), after R1b it supersedes; another backbuffer texture, guard failure (each counted as a decline cause), no backbuffer UI draw; R1 == R1b frames, and re-armed frames, identical with the key on and off (one extra copy on a re-armed frame)'
    Sources     = @('ui_live_anchor_warp.cpp', '{repo}/OptiScaler/ext/reshade/ReShadeBoundaryCore.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Libs        = @('d3d11.lib', 'dxgi.lib', 'd3dcompiler.lib')
    Defines     = @('NOMINMAX')
}
