@{
    Class   = 'cpu'
    Note    = '[UP] IFGFeature::GetIndexWillBeDispatched mirrors the resync branch of GetDispatchIndex (sliced production text, table test, negative control)'
    Runner  = 'run_dispatch_index_predictor.ps1'
    Sources = @('dispatch_index_predictor.cpp')
}
