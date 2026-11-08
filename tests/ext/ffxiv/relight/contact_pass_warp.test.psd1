@{
    Class       = 'warp'
    Note        = 'Contact shadow pass on D3D11 WARP with the real shaders and the real Pass class on synthetic buffers: a wall within reach darkens the ground beside it, a wall beyond the march length does not, open ground and a NaN pixel stay lit, the sign key and a sun pointing away darken nothing, full strength reaches zero, a half-pixel render window smaller than the allocation is passed through outside and shadowed inside, a quarter-turn camera with a world-space sun applies the view transform, the march covers the render window half and no more (read back from its own target), a neighbour across a depth edge is not blended into a pixel, a march shorter than the start distance finds nothing while the same march without the rule finds the surface at the receiver edge, and the pixel normal ignores a cleared neighbour beside it'
    Prepare     = 'prepare_contact.ps1'
    Sources     = @('contact_pass_warp.cpp', '{repo}/OptiScaler/ext/ffxiv/relight/ContactShadowsPass.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler', '{out}')
    Libs        = @('d3d11.lib', 'dxgi.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
