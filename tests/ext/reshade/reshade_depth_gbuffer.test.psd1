@{
    Class       = 'cpu'
    Note        = 'Automatic ReShade G-buffers, the binder slots against the fake ReShade backend: the size rule per buffer (remap an output-sized source only), the request the backend gets, bind and restore at finish only if still ours, the unverified undo (erase, slot stops until reload), a failed restore retried, reference balance, nothing without a bound depth, zero extra backend calls without G-buffer variables, the option going off releases the targets, the add-on rebind observation'
    Sources     = @('reshade_depth_gbuffer.cpp', '{repo}/OptiScaler/ext/reshade/ReShadeDepthCore.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Defines     = @('NOMINMAX', 'UNDEBUG')
    Std         = 'c++latest'
}
