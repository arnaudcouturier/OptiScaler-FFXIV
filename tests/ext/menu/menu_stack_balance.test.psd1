@{
    Class       = 'cpu'
    Note        = 'ImGui stack balance of every fork menu function (OptiScaler/ext, any file that calls ImGui): BeginDisabled/EndDisabled, PushItemWidth/PopItemWidth, PushStyleColor/PopStyleColor (count argument), PushStyleVar, Indent/Unindent, PushID/PopID and friends are as many as their closes and no return can run between them; the named card functions are also run against broken copies (first close removed, return injected after the first open)'
    Sources     = @('menu_stack_balance.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
