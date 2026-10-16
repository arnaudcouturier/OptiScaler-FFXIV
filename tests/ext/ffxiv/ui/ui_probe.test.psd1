@{
    Class       = 'cpu'
    Note        = 'S10-A: ReShade/Dalamud probe on the test process: module list in load order, module ownership, inline-jump decoder (E9, EB, FF 25, mov+jmp), call chain text, module diff'
    Sources     = @('ui_probe.cpp', '{repo}/OptiScaler/ext/ffxiv/ui/UiProbe.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
