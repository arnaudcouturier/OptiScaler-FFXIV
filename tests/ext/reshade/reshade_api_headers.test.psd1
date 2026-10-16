@{
    Class       = 'cpu'
    Note        = 'S10 D0: vendored ReShade v6.8.0 add-on API headers build (C++20, /W4), API version 20, event signatures the probe relies on'
    Sources     = @('reshade_api_headers.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
