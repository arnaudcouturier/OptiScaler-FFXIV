@{
    Class       = 'cpu'
    Note        = 'FG recorder rearm: the sticky start failure clears when the key arms on the real tap path, so a start that failed after the key went off cannot block the next session (the taps run in the order a player produces them; a latched first tap fails here)'
    Sources     = @('fgrec_rearm.cpp')
    IncludeDirs = @('stubs', '.', '{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
