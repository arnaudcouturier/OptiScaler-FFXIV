@{
    Class       = 'cpu'
    Note        = 'Live FG output switch source pins: one-class hook install and its ten slots, the release hook pass-through and both special branches, the bridge release order, the pure headers'
    Sources     = @('fg_switch_pins_text.cpp')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
