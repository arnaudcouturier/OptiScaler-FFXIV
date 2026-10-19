@{
    Class       = 'cpu'
    Note        = 'The menu text scanner on its own: StripComments (character literals, digit separators, raw strings reported, #if 0), function bodies and reachability, ImGui stack balance counters, each with negative controls'
    Sources     = @('menu_scan_self.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
