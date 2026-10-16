@{
    Class       = 'cpu'
    Note        = 'E5 pure CreateSwapChainForComposition pass-through decision, incl. the Companion (ext::core::BypassScope) case'
    Sources     = @('composition_passthrough_test.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('UNDEBUG')
}
