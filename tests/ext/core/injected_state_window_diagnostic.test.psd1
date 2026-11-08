@{
    Class       = 'cpu'
    Note        = 'Bounded private-state window probe counts only foreign-thread native draws on the exact active context and token'
    Sources     = @('injected_state_window_diagnostic.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Libs        = @('d3d11.lib')
    Defines     = @('NOMINMAX')
}
