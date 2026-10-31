@{
    Class       = 'cpu'
    Note        = 'Live FG output switch probation: 20 presents or 8 s, the stall rule, three-failure watchdog, a lost device never masked, a paused provider never rolled back'
    Sources     = @('fg_switch_probation.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
