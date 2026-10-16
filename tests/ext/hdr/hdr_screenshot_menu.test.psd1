@{
    Class       = 'cpu'
    Note        = 'F4 Include OptiScaler menu: when the HDR PNG is taken after the menu is drawn (pure decision logic)'
    Sources     = @('hdr_screenshot_menu.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Std         = 'c++20'
}
