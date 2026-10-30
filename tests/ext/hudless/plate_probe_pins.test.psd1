@{
    Class       = 'cpu'
    Note        = 'Plate probe wiring pins: draw hooks note their arguments behind one atomic test, the indexed hook order, the vertex shader stamp, the probe arms the boundary detector, frame hand-over, multiplier note after the selector and the interlock, ini-only keys, defaults false'
    Sources     = @('plate_probe_pins.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
