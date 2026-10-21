@{
    Class       = 'cpu'
    Note        = 'Native-quality install transaction against the REAL detours.lib (ext/ffxiv/quality/NativeQualityTransaction.h): an exited thread fails the enlist and FilterLive removes it, another thread''s open transaction is reported not aborted, a failing attach aborts and names its stage, a second attempt works after a failure on a poisoned pointer'
    Sources     = @('native_quality_detours_test.cpp', '{repo}/OptiScaler/library/detours/detours.lib')
    IncludeDirs = @('{repo}/OptiScaler', '{repo}/OptiScaler/include')
    Defines     = @('NOMINMAX', 'UNDEBUG')
    Std         = 'c++latest'
}
