@{
    Class       = 'cpu'
    Note        = 'DLSS-G preset guards: the small x86-64 decoder (PresetX64.h) - lengths, control flow, register and memory writes of the instructions used by NVIDIA create function, everything unknown refused'
    Sources     = @('fg_preset_x64.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
