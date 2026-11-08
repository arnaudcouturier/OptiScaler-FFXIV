@{
    Class       = 'cpu'
    Note        = 'Reflections frame-exact A/B wiring pins: called right after the swap guard and around the draw path for a served reader only, the extra draw inside the explicit-state guard with a minimal spec and the tessellation stages never neutralised, depth and stencil writes off in its depth-stencil copy, the order guard / engine map in / copy of the destination / write-off state / draw, replay and own-work markers around it, the gate reading the open mask first, the capture''s one draw and its three places of their own after the stage section, no key'
    Sources     = @('ssr_ab_pins.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
