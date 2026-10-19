@{
    Class       = 'cpu'
    Note        = 'SourceTracker (draw each shared menu function once per tab pass): claim rules, null source, capacity, a model of the upstream tab loop (old logic draws twice around a fork card, new logic once), text pins on the loop in menu_common.cpp'
    Sources     = @('source_tracker.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
