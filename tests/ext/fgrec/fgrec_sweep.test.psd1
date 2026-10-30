@{
    Class       = 'cpu'
    Note        = 'FG recorder session cleanup: the newest 5 own-pattern sessions stay with their rotations while older ones go, and foreign files (backups, lookalikes, other logs) are never touched (a widened pattern or wrong cap fails)'
    Sources     = @('fgrec_sweep.cpp')
    IncludeDirs = @('stubs', '.', '{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
