@{
    Class       = 'warp'
    Note        = 'Atmosphere''s camera sources (CAMFIX) on D3D11 WARP with the real FrameReader (GodraysSun.cpp) as the stage instantiates it twice, on the user''s three live camera tables and the benchmark''s: the light draws'' copy reads each table back verified (the benchmark''s too: the lighting layout with no sun-shaped light draw), with the inverse projection, view rows and inverse view row bit-equal to the capture''s words; the two copies of one buffer are bit-equal in every field the hold folds; a frame whose only copy is the light draws'' one is folded as the light draws'' source, with both as the sun draw''s, with neither as no camera; zeros, NaN, a damaged view-projection and a wrong w row land and are refused (no camera taken from them; a refused sun draw table is rescued by a good light draws'' one); the light draws'' ring returns the newest frame before and never goes back'
    Sources     = @('atmo_camera_warp.cpp', '{repo}/OptiScaler/ext/ffxiv/engine/GodraysSun.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Libs        = @('d3d11.lib', 'dxgi.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
