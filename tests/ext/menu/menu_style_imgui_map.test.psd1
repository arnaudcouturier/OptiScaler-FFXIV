@{
    Class       = 'cpu'
    Note        = 'Menu style: the ImGui adapter assigns every ImGuiCol_ the bundled library declares and every Ember palette member (negative controls included)'
    Sources     = @('menu_style_imgui_map.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
