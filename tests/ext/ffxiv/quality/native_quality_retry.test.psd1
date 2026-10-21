@{
    Class       = 'cpu'
    Note        = 'Native-quality hook install state machine (ext/ffxiv/quality/NativeQualityRetry.h): install once and cache, one step at a time, no clock on the fast path, fail open on a throw'
    Sources     = @('native_quality_retry_test.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('UNDEBUG')
}
