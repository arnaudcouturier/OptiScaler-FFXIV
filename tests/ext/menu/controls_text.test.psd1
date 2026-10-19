@{
    Class       = 'cpu'
    Note        = 'WP4 controls of the menu redesign as source-text pins: per control the widget, the config write inside its own widget block, the restart-or-live wording, the fold it lives in; each row also run against broken copies (HUD-less ReShade presets and UI recomposition, XeFG extras, NR Advanced fold, Character preview Advanced keys, RTX 20/30 tuning fold)'
    Sources     = @('controls_text.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
