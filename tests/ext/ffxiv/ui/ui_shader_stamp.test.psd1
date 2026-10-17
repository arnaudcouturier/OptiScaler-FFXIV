@{
    Class       = 'cpu'
    Note        = 'S10-E3: creation-time stage stamp without arming: every catalog program (5 scene + 7 UI) is stamped with registry=false and read back by the tracker''s own reader; non-catalog programs get no SetPrivateData; the epoch advances on every creation; zero heap allocations and no D1 registry on that path; registry=true adds the per-program record'
    Sources     = @('ui_shader_stamp.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Libs        = @('dxguid.lib')
    Defines     = @('NOMINMAX')
}
