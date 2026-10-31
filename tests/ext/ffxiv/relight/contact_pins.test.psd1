@{
    Class       = 'cpu'
    Note        = 'Contact shadow wiring pins: one relaxed load on the draw path and the two call sites, the pass replacing only the draw call (not the rest of the hook), the game''s shader resource restored by a scope guard, the sun read once per session and no game resource ever written, two draws in a private device-context state, the pure rules staying pure, and every key off by default, documented, bound and with a control or a stated reason'
    Sources     = @('contact_pins.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
