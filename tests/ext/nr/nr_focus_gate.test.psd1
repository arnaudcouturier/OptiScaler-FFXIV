@{
    Class       = 'cpu'
    Note        = 'NR focus-loss gate: off by default, debounce both ways, minimise, flicker, switch toggled while unfocused (NrFocusGate.h)'
    Sources     = @('nr_focus_gate.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
}
