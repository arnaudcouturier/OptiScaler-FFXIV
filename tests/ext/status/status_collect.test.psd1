@{
    Class       = 'cpu'
    Note        = 'status.json collector: feature states and reasons, runtime versions with the provisional-text gate, folder labels, loader-list gating, cached adapter'
    Sources     = @('status_collect.cpp', '{repo}/OptiScaler/ext/status/StatusJson.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Libs        = @('dxgi.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
