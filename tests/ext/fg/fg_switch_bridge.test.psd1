@{
    Class       = 'cpu'
    Note        = 'Bridge side of the live output switch: the two present seams answer the bridge''s own value while no switch is composed, an armed swap runs the switch point, and the colour-space and resize records decide the replay'
    Sources     = @('fg_switch_bridge.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
