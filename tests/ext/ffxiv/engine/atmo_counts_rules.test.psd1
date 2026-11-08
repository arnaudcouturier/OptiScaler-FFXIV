@{
    Class       = 'cpu'
    Note        = 'Atmosphere''s timed counts line, rules on the host (INT2): the cadence (the first look only starts the window, nothing due before ten seconds, due at ten, a clock that went backwards starts again, a taken line starts the next window), the tallies per entry (own seam / chained hook / owner hook), per decline reason and per skip reason with out-of-range reasons dropped, the window and the running total across lines (a window nobody reached still writes its line), and the line''s text (the entries, only the reasons that happened, the rendered frames, the settings, every reason with its own key)'
    Sources     = @('atmo_counts_rules.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
