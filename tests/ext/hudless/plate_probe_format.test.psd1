@{
    Class       = 'cpu'
    Note        = 'Plate probe text formats (ext/hudless/PlateProbeFormat.h): csv escaping, outputs.csv row, summary line, rule R0 (every condition and the depth function), draws.csv row, sampling and rewrite schedules, byte caps, per-shader table, run folder pruning (5)'
    Sources     = @('plate_probe_format.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
