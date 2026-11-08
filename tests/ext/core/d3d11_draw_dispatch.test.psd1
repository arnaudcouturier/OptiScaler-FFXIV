@{
    Class       = 'cpu'
    Note        = 'Current-entry dispatch uses a typed ABI, unchanged entries keep the trampoline, and fresh-entry and per-layer scopes unwind and isolate context/API'
    Sources     = @('d3d11_draw_dispatch.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Libs        = @('d3d11.lib')
    Defines     = @('NOMINMAX')
}
