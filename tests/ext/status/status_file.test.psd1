@{
    Class       = 'cpu'
    Note        = 'status.json writer: single-owner lock, atomic replace, sweep, own-file deletion, error episodes, series, throttled cycle, shutdown'
    Sources     = @('status_file.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Libs        = @('dxgi.lib', 'psapi.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
