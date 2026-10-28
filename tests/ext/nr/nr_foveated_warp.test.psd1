@{
    Class       = 'cpu'
    Note        = 'NR foveated profile: packed distance vs a numerical integral, pack/unpack round trip, identity and clamp rules, sizes, pixel share, 5% auto-off, shader constant slots (FoveatedWarp.h)'
    Sources     = @('nr_foveated_warp.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
