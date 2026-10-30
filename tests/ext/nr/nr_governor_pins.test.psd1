@{
    Class       = 'cpu'
    Note        = 'NR frame-rate governor wiring pins (keys and defaults, one-place cap, off path, live count, once-per-build probe, VRAM mark, expected-stall seam, override toast, capped readout)'
    Sources     = @('nr_governor_pins.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
    Std         = 'c++20'
}
