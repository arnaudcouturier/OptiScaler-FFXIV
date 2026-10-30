@{
    Class       = 'cpu'
    Note        = 'Native nameplate wiring pins: the DrawIndexed intercept and the pixel-shader stamp, the three seam consumers and the multiplier note, the snapshot-before-bit order at load and in the menu (never a clear from either), the engine own bit sink, and no plugin-era plate machinery'
    Sources     = @('plate_wiring_pins.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
