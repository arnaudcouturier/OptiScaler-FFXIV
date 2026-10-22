@{
    Class       = 'cpu'
    Note        = 'DLSS-G preset menu sentences: "Chosen: X, in use: Y" with the reason (pending, restart needed, control off); in use is only said for a preset the runtime confirmed'
    Sources     = @('fg_preset_menu_text.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
