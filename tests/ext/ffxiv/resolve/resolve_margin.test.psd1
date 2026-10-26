@{
    Class       = 'cpu'
    Note        = 'offline uniqueness margin (tools/ffxiv-resolve/Margin.h): nearest other site in fixed bytes, flags a unique pattern within 1-2 bytes of another site, masks, scope, chunk borders, exclusions'
    Sources     = @('resolve_margin.cpp')
    IncludeDirs = @('{repo}/OptiScaler', '{repo}/tools/ffxiv-resolve')
    Defines     = @('NOMINMAX')
}
