@{
    Class = 'cpu'
    Note = 'SSR pair capture bounds, timeout, two-slot lifecycle and manifest/file naming contract'
    Sources = @('ssr_pair_rules.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args = @()
    Std = 'c++20'
}
