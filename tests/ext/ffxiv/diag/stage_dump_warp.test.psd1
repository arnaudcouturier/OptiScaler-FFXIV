@{
    Class       = 'warp'
    Note        = 'Per-stage image dump over a D3D11 WARP device: a capture frame with a synthetic two-stage chain writes the chain input and both outputs (cropped to the render window, the right bytes, the second input as a reference), a non-capture frame writes nothing and allocates nothing, the budget skips with a facts line and a stage image displaces the atlas, and every stage gets its status line (ran / held / combined / declined / not armed / not reached)'
    Sources     = @('stage_dump_warp.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Libs        = @('d3d11.lib', 'dxgi.lib', 'user32.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
