@{
    Class       = 'cpu'
    Note        = 'Window mode: [Window] Mode parsing (off, probe, on; anything else is off)'
    Sources     = @('window_mode_parse.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
}
