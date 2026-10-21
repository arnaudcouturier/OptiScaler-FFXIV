@{
    Class       = 'cpu'
    Note        = 'NR focus pause for two consumers (scene NR, character preview NR): one shared window sample, one FocusGate each; independence, sample sharing, and the wiring of the preview run (NrFocusTracker.h, NrFocus.cpp, CharaNr.cpp)'
    Sources     = @('nr_focus_tracker.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
}
