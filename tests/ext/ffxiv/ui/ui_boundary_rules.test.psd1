@{
    Class       = 'cpu'
    Note        = 'S10-A: boundary rules R1/R2/R3 on synthetic draw sequences (pre-hook shaders, fades, menu-only, no UI shader, UI before the last stage); online tracker equals the retrospective definition'
    Sources     = @('ui_boundary_rules.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
}
