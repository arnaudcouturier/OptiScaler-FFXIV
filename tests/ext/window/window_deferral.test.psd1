@{
    Class       = 'cpu'
    Note        = 'Window mode: the deferral state machine (Idle/Pending/Replaying, keep the latest, soft and hard wait limits) on a fake clock'
    Sources     = @('window_deferral.cpp')
    IncludeDirs = @('.', '{repo}/OptiScaler')
}
