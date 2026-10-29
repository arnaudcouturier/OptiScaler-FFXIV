@{
    Class       = 'cpu'
    Note        = 'NR depth-aware enlargement: bilateral weights, taps, fallbacks, depth texel mapping, shader slots, decision and status (DepthAwareWeights.h)'
    Sources     = @('nr_depth_aware.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
