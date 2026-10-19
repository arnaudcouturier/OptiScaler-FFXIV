@{
    Class       = 'cpu'
    Note        = 'Window mode step 2 on fakes: the border fix is armed by the leave, gated on DLSS-G confirmed off, owner thread only, and never touches a real windowed mode'
    Sources     = @('window_fix_flow.cpp')
    IncludeDirs = @('.', '{repo}/OptiScaler')
}
