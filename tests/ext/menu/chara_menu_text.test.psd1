@{
    Class       = 'cpu'
    Note        = 'Character preview card layout: title == picker name, user switches and states before the fold, ONE closed Advanced (debug) fold (plain TreeNode, balanced TreePop, no return) holding every counter and debug aid, each key written from exactly one checkbox, the off-reason stays in the main view (source text pins, negative controls)'
    Sources     = @('chara_menu_text.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
