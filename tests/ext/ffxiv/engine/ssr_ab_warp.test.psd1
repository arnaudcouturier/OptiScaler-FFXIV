@{
    Class       = 'warp'
    Note        = 'Reflections frame-exact A/B (ext/ffxiv/engine/SsrAb.cpp) on D3D11 WARP, with a tessellated fake water draw that adds the reflection map''s rgb and dims a probe colour by 1/(1+alpha)^2, blended additively onto a float target with a D24S8 buffer in a state that would write depth and stencil: the extra draw is issued with the engine''s map into a private copy of the target made immediately before (an additive blend sees the same destination), the real draw with this module''s map, the two pictures differ exactly by the formula texel by texel, the extra draw writes neither depth nor stencil while the real one does, the tessellation stages stay bound, the explicit-state guard puts back the render target, the depth view, the depth-stencil state with its reference, the reflection slot and the pipeline, the extra draw is a replay and own work while the real draw is neither, the capture''s one draw is taken once, a closed capture and an unserved reader never ask, and a second target, an output-merger UAV, no target, a multisampled target and the SSR diagnostic each leave the draw as the game made it with a counted reason, a facts note and one log line a session'
    Sources     = @('ssr_ab_warp.cpp',
                    '{repo}/OptiScaler/ext/ffxiv/engine/SsrAb.cpp',
                    '{repo}/OptiScaler/ext/ffxiv/engine/SsrApply.cpp',
                    '{repo}/OptiScaler/ext/ffxiv/engine/SsrPass.cpp',
                    '{repo}/OptiScaler/ext/ffxiv/engine/SsrSwap.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler', '{out}')
    Args        = @('{repo}')
    Libs        = @('d3d11.lib', 'dxgi.lib', 'd3dcompiler.lib', 'user32.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
    Prepare     = 'prepare_ssr.ps1'
}
