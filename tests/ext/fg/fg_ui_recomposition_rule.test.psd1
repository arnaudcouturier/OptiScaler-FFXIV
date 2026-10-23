@{
    Class       = 'cpu'
    Note        = 'UI recomposition rule: requested iff HUD-less recomposition AND the active preset is Auto or B (A and C drop it), decided from the request with no runtime confirmation so the first SetOptions is final; an unavailable control runs as Auto; live switch transitions keep the old value until the commit and the new one at the creation'
    Sources     = @('fg_ui_recomposition_rule.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
