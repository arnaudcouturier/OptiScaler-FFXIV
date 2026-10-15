@{
    Class       = 'cpu'
    Note        = 'XeFG MFG unlock byte patches, pacing deadlines, dynamic factor, camera view (D3), real ext/fg/xefg headers'
    Sources     = @('xefg_mfg_integration.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Defines     = @('UNDEBUG')
}
