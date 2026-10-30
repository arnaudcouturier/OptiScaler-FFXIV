@{
    Class       = 'cpu'
    Note        = 'status.json source pins: throttle constants, ownership branches, error steps, shutdown shape, loader gating, separators, wording'
    Sources     = @('status_pins.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
}
