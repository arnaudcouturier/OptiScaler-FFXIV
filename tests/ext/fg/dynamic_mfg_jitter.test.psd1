@{
    Class       = 'cpu'
    Note        = 'Dynamic MFG selector with a seeded +-10% jitter plant a few percent inside the up and wide-down thresholds: 5 minutes, no flapping'
    Sources     = @('dynamic_mfg_jitter.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
}
