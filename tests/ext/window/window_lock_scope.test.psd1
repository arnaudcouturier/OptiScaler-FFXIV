@{
    Class       = 'cpu'
    Note        = 'Window mode (L2): the module lock scope around the live out-calls on a fake mutex (a nested entry inside a call out never unlocks more than the thread owns)'
    Sources     = @('window_lock_scope.cpp')
    IncludeDirs = @('.', '{repo}/OptiScaler')
}
