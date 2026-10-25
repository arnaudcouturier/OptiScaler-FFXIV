@{
    Class       = 'cpu'
    Note        = 'Automatic ReShade depth: the event dispatcher over the real binder and a fake ReShade world (every event reaches the binder, the callbacks do not look at the enabled flag, the frame counter ticks once per finish on our device and on refused passes, capture and begin agree on it, the runtime registry, nothing escapes)'
    Sources     = @('reshade_depth_events.cpp', '{repo}/OptiScaler/ext/reshade/ReShadeDepthCore.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Defines     = @('NOMINMAX', 'UNDEBUG')
    Std         = 'c++latest'
}
