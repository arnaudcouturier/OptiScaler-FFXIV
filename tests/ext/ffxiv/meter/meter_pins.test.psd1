@{
    Class       = 'cpu'
    Note        = 'Effect meter (METER) wiring pins, read from the sources: the stage-dump seam tells the meter on every frame before the capture''s gate (function pointers, null where the meter is not linked), the pass is started from one place behind the schedule''s yes and never from the counting entry, nothing waits or flushes, the draws are own work, the pass touches only what the explicit-state guard covers (two pixel-shader resources, one constant buffer, no vertex-shader resource), one draw and a corner copy, the shader and the rules share the tap hash, the thresholds and the register layout, every chain stage hands its white point over with its run report, every effect card ends with the meter''s line, the log lines are the rules'' formatters on the rules'' clocks, and no config key exists'
    Sources     = @('meter_pins.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
