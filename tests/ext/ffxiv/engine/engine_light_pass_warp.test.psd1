@{
    Class       = 'warp'
    Note        = 'Engine light controls, the exposure hold''s pass on D3D11 WARP with the real shader and the real Pass class on a synthetic 160x96 frame: the auto hold multiplies the stored scene by s_hold / s_now with s_now read from the GPU copy of g_CommonParameter (a 1.3x take-back comes back to the held picture within the half-float rounding, alpha is passed through bit for bit), the gain is bounded to 0.5..2, a copy that fails the fingerprint y = max(1, 2.04 s^2), a NaN and an out-of-band exposure are gain 1 with the input''s own bits, the manual factor scales exactly, every identity (mode off, no copy, no hold, a factor of 1) is no pass with no failure and no draw, the pass covers the render window and a two-pixel margin and writes nothing beyond, the game''s render target, t0, constant buffers, rasterizer / blend (factor and mask) / depth state and viewport come back after a run, the timing pairs balance, and every binding the pass cannot read returns null with a reason'
    Prepare     = 'prepare_engine_light.ps1'
    Sources     = @('engine_light_pass_warp.cpp', '{repo}/OptiScaler/ext/ffxiv/enginelight/EngineLightPass.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler', '{out}')
    Libs        = @('d3d11.lib', 'dxgi.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
