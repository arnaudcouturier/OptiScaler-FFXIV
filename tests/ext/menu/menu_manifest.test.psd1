@{
    Class       = 'cpu'
    Note        = 'Menu manifest: layout card ids unique and registered once, tabs real, FFXIV-only cards exactly the FFXIV tab, anchors resolve, no fork card inside a shared-function group in the final order, released ids resolve, aliases, ini-only keys, seam line budget; every rule also run on a broken model (source text scan, comments stripped)'
    Sources     = @('menu_manifest.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
