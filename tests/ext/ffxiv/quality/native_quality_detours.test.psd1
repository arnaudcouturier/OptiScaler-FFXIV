@{
    Class       = 'cpu'
    Note        = 'Native-quality install transaction against the REAL detours.lib (ext/ffxiv/quality/NativeQualityTransaction.h): an exited thread fails the enlist and FilterLive removes it, another thread''s open transaction is reported not aborted, a failing attach aborts and names its stage, a second attempt works after a failure on a poisoned pointer'
    Sources     = @('native_quality_detours_test.cpp', '{repo}/OptiScaler/library/detours/detours.lib')
    IncludeDirs = @('{repo}/OptiScaler', '{repo}/OptiScaler/include')
    Defines     = @('NOMINMAX', 'UNDEBUG')
    # The product builds with /EHsc /O2 /Ot /GL. Optimised, "/EHsc" drops a try/catch that only guards Detours' extern "C" calls, so the exception tests
    # must run with the same flags (at /Od they would pass even where the product's handler is gone). Raw flags go through Libs.
    Libs        = @('/O2', '/Ot', '/GL')
    Std         = 'c++latest'
}
