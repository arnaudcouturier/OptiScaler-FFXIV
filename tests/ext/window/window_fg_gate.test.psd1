@{
    Class       = 'cpu'
    Note        = 'Window mode: the "DLSS-G confirmed off" gate (one complete present with frame generation inactive, DLSS-G output only)'
    Sources     = @('window_fg_gate.cpp')
    IncludeDirs = @('.', '{repo}/OptiScaler')
}
