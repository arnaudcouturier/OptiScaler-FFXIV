@{
    Class       = 'warp'
    Note        = 'Atmosphere''s pass on D3D11 WARP with the real shader and the real Pass class on synthetic buffers: the mix equals the CPU reference (AtmoRules.h''s own functions in linear light) per pixel within two half steps, the near field and the sky are the scene''s own texels bit for bit, the window''s outside is the handed-in picture bit for bit, a key that says nothing to do answers a byte-exact copy with no draw, no sun runs on the ambient term alone and a mirrored sun mirrors the haze, two runs are one picture bit for bit, the engine-fog and night scales reach the pass through the density, the game''s bindings come back after a run, and a binding the pass cannot read returns null with a reason'
    Prepare     = 'prepare_atmo.ps1'
    Sources     = @('atmo_warp.cpp', '{repo}/OptiScaler/ext/ffxiv/engine/AtmoPass.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler', '{out}')
    Libs        = @('d3d11.lib', 'dxgi.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
