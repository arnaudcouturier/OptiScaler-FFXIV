@{
    Class       = 'warp'
    Note        = 'Shadow mapping''s mask route and light colour feed through the REAL relight draw seam (Inject.cpp OnGameDraw) and the REAL shadows dispatch on D3D11 WARP, with Contact shadows OFF: lift 0.5 reaches LightDrawRouteSink with a non-null layout and binds the lifted copy for exactly one game draw; lift 0 with ambient 2.0 ticked stages the light colour, the colour lands and the ambient provider answers its two entries at the curve (the capture session''s night colour gives 1.0); with Contact shadows off nothing of the contact pass ever runs; with both keys at identity no sink, copy or entry is asked; a route that stood down is not asked again; the game''s draw runs exactly once on every path; the once-per-state log lines (route first draw, first colour read, each idle reason) and the card''s counters; the colour ring is keyed by capture count and survives a frame counter stepping by 4'
    Prepare     = 'prepare_shadows.ps1'
    Sources     = @('shadows_route_warp.cpp',
                    '{repo}/OptiScaler/ext/ffxiv/relight/Inject.cpp',
                    '{repo}/OptiScaler/ext/ffxiv/relight/ReflectionIndex.cpp',
                    '{repo}/OptiScaler/ext/ffxiv/relight/ContactShadowsApply.cpp',
                    '{repo}/OptiScaler/ext/ffxiv/relight/ContactShadowsPass.cpp',
                    '{repo}/OptiScaler/ext/ffxiv/relight/ContactShadowsDump.cpp',
                    '{repo}/OptiScaler/ext/ffxiv/shadows/ShadowsDispatch.cpp',
                    '{repo}/OptiScaler/ext/ffxiv/shadows/ShadowsPass.cpp',
                    '{repo}/OptiScaler/ext/core/ExtConfig.cpp', '{repo}/OptiScaler/ext/hdr/HdrConfig.cpp',
                    '{repo}/OptiScaler/ext/hudless/HudlessConfig.cpp', '{repo}/OptiScaler/ext/hudless/HudlessState.cpp')
    IncludeDirs = @('route_stubs', '{repo}/tests/ext/hdr/stubs', '{repo}/OptiScaler', '{repo}/external/simpleini', '{out}')
    Libs        = @('d3d11.lib', 'dxgi.lib', 'd3dcompiler.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
