@{
    Class       = 'warp'
    Note        = 'Engine light controls, the lamp arm (ENG2) on D3D11 WARP with the real EngineLightLamps.cpp: a point-light volume draw (additive ONE/ONE/ADD on two R11G11B10 targets, a light-bit shader) puts factor x its light into BOTH light buffers through the blend factor, a factor of 1 builds no wrapper and touches no state, the game''s blend state, blend factor and sample mask come back bit for bit also when the draw throws, the directional light (not blended), a one-target additive draw, a blended pair in another format, an R8 MAX reader, an independent state whose second target is not additive and a draw without the light bit are all left alone, the wrapper composes with a reader swap around the draw, one derived state serves every volume draw, and the capture frame writes the whole bound g_LightParam of each volume draw (no string on any other frame)'
    Sources     = @('engine_light_lamps_warp.cpp', '{repo}/OptiScaler/ext/ffxiv/enginelight/EngineLightLamps.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Libs        = @('d3d11.lib', 'dxgi.lib', 'd3dcompiler.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
