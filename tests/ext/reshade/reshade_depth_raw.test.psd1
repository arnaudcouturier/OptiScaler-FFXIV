@{
    Class       = 'cpu'
    Note        = 'Automatic ReShade depth, the raw layer over effect_runtime (the only file that calls into ReShade): garbage handles and exceptions inside ReShade give false instead of a crash, and the argument wiring (update_texture_bindings order, readback order, uniform pointer overloads with array index 0, size query first, the source annotation, per-effect definitions, back buffer 0, enumeration callbacks that only count) against a fake runtime and device derived from the vendored abstract classes'
    Sources     = @('reshade_depth_raw.cpp', '{repo}/OptiScaler/ext/reshade/ReShadeDepthRaw.cpp', '{repo}/OptiScaler/ext/reshade/ReShadeD3D11.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler', '.')
    Libs        = @('d3d11.lib', 'dxgi.lib')
    Defines     = @('NOMINMAX', 'UNDEBUG')
    Std         = 'c++latest'
}
