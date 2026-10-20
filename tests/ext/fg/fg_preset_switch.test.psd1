@{
    Class       = 'cpu'
    Note        = 'DLSS-G live preset switch state machine: step order, failure at every step keeps the old request and says restart needed, no retry loop, Auto needs a known original, invalid request rejected'
    Sources     = @('fg_preset_switch.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
