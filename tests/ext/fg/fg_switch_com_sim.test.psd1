@{
    Class       = 'cpu'
    Note        = 'Live FG output switch against refcounted COM fakes: release hook in both modes, both library models, the witness, per-class hooks, the factory re-entry, every direction'
    Sources     = @('fg_switch_com_sim.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
