@{
    Class       = 'cpu'
    Note        = 'Hook rebind: a real Detours round trip on a dummy vtable (attach, drain the gate, detach the trampolines, null the originals), a busy body that stops the detach, and a set with nothing attached'
    Sources     = @('fg_switch_hookrebind.cpp')
    IncludeDirs = @('{repo}/OptiScaler', '{repo}/OptiScaler/include')
    Libs        = @('{repo}/OptiScaler/library/detours/detours.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
