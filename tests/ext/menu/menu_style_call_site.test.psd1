@{
    Class       = 'cpu'
    Note        = 'Menu style: the one upstream line, ext::menu::ApplyStyle in ApplyThemeStyle after the classic colours and before the HDR tone mapping, once, with no new include (positional, with negative controls)'
    Sources     = @('menu_style_call_site.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
