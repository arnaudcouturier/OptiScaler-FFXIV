@{
    Class       = 'cpu'
    Note        = 'Advanced (FFXIV) card: registered as ffxiv_advanced in the FFXIV tab with a predicate that tests the game build, both keys (RenderSizeFromWidthHeight, ResetOnUpscalerChange) read and written only from their own checkbox inside one closed fold with a balanced TreePop, tooltips say FFXIV only and when a change applies, the Hotfix ini fragment says FFXIV only (source text pins, negative controls)'
    Sources     = @('advanced_menu_text.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
