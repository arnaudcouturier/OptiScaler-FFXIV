@{
    Class       = 'warp'
    Note        = 'Shadow mapping''s mask lift on D3D11 WARP with the real shaders and the real pass classes on synthetic buffers: the 0.5 lift answers the CPU reference of the squared-mask remap over the whole buffer (the copy carries the square root, half a level of slack at the R8 grid), a lift at the identity answers null with no D3D object, the tent on a step edge is the CPU kernel over the REMAPPED field, the light-draw fixture reads the lifted copy from the module''s bound slot and the restore puts the game''s own resource back at the slot, the contact pass''s own combine integrates the lift without a second copy, second runs create no D3D object, and the GPU timing pair''s ends never exceed its begins'
    Prepare     = 'prepare_shadows.ps1'
    Sources     = @('shadows_warp.cpp', '{repo}/OptiScaler/ext/ffxiv/shadows/ShadowsPass.cpp', '{repo}/OptiScaler/ext/ffxiv/relight/ContactShadowsPass.cpp', '{repo}/OptiScaler/ext/ffxiv/relight/ContactShadowsDump.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler', '{out}')
    Libs        = @('d3d11.lib', 'dxgi.lib', 'd3dcompiler.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
