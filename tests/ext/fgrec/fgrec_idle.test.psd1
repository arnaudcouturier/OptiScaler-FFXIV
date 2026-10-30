@{
    Class       = 'cpu'
    Note        = 'FG recorder idle writer: auto-reset wake and no flush on an empty batch (a spinning writer burns a core here)'
    Sources     = @('fgrec_idle.cpp')
    IncludeDirs = @('stubs', '.', '{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
