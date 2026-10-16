@{
    Class       = 'warp'
    Note        = 'S10-A: a 1000-frame D1 session on WARP against an in-memory sink: every file produced on its schedule with parseable contents, anomaly rate limit, byte budgets with one TRUNCATED marker'
    Sources     = @('ui_diag_session.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Libs        = @('d3d11.lib', 'dxgi.lib', 'd3dcompiler.lib')
    Defines     = @('NOMINMAX')
}
