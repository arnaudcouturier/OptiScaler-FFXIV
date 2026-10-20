@{
    Class       = 'cpu'
    Note        = 'Menu style: the Ember palette (every slot set, neutrals never take the accent, the accent only marks state, alpha, contrast, metrics)'
    Sources     = @('ember_palette.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Std         = 'c++20'
}
