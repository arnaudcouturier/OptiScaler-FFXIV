@{
    Class       = 'cpu'
    Note        = 'Automatic ReShade depth, the live backend over a fake raw layer: size-query-first strings (NUL stripped, never truncated, inconsistent answers fail), tri-state definitions, handle lists that outgrow the first capacity, untrusted views, SameDevice (native devices, the only-runtime fallback while no capture exists, never another device once one does), the runtime registry, the first-bind readback log, and the real binder on top with a fault at every raw call of a pass'
    Sources     = @('reshade_depth_backend.cpp', '{repo}/OptiScaler/ext/reshade/ReShadeDepthBackend.cpp', '{repo}/OptiScaler/ext/reshade/ReShadeDepthCore.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Defines     = @('NOMINMAX', 'UNDEBUG', 'DEPTH_TEST_LOG')
    Std         = 'c++latest'
}
