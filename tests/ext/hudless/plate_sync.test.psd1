@{
    Class       = 'cpu'
    Note        = 'Plate locks in the test build: a leaf re-lock is reported instead of deadlocking, the leaf count is per thread, the pass mutex does not count as a leaf, two threads exclude each other, scopes always release'
    Sources     = @('plate_sync.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('PLATES_CHECKED_MUTEX', 'NOMINMAX')
    Std         = 'c++20'
}
