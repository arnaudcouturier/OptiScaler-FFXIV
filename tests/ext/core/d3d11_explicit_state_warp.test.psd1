@{
    Class       = 'warp'
    Note        = 'The explicit pipeline-state guard (ext/core/D3D11ExplicitState.h) on D3D11 WARP: with every category declared, Neutralise() sets HS, DS, GS and the predicate to none and Restore() brings back a full getter snapshot (shaders, resource slots, samplers, constant-buffer ranges, rasterizer state, two viewports and scissors, blend factor and sample mask, stencil reference, render target, the UAV with its counter of 5, predicate) exactly; the destructor restores on every exit including an exception, Restore() runs once, only the declared categories are read or written (an undeclared sampler stays where the pass left it, HS/DS/GS stay bound when the pass does not ask to neutralise them), OmSnapshot alone puts a render target and a UAV back after OMSetRenderTargets(0) dropped it, and a game state without HS/DS/GS, predicate or UAV is left as it was; FullStateSnapshot puts the SRVs of each stage back in ONE setter call [0, hi] (hi = the highest slot bound at capture or after the stand-in) and in no call when the stage is unchanged, so the remembered slot ranges of a range-tracking hook stay small (a recording layer of real Detours hooks on the six SetShaderResources entries pins the ranges; a high slot added by the stand-in comes back null, a high slot of the game comes back)'
    Sources     = @('d3d11_explicit_state_warp.cpp', '{repo}/OptiScaler/library/detours/detours.lib')
    IncludeDirs = @('{repo}/OptiScaler', '{repo}/OptiScaler/include')
    Libs        = @('d3d11.lib', 'dxgi.lib', 'd3dcompiler.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
