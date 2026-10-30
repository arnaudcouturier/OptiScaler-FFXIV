@{
    Class       = 'cpu'
    Note        = '[BRIDGE] source pin on the D3D11 -> D3D12 hand-over: one guarded FlushAfterSignal seam, right after the accepted fence Signal and its error check, before the D3D12 wait, with negative controls for a moved call and a lost guard'
    Sources     = @('bridge_upstream_pin_test.cpp')
    Args        = @('{repo}')
    Std         = 'c++latest'
}
