@{
    Class       = 'cpu'
    Note        = 'S10 D0: RegisterAddon degrades cleanly with a null own module, a null ReShade module and a module without the add-on exports'
    Sources     = @('reshade_registration.cpp', '{repo}/OptiScaler/ext/reshade/ReShadeRegister.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Defines     = @('NOMINMAX', 'UNDEBUG')
    Std         = 'c++latest'
}
