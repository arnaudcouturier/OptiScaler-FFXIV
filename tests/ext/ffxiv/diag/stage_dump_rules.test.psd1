@{
    Class       = 'cpu'
    Note        = 'Per-stage image dump, the pure rules: the stage registry and file names (chain order), the rank between the engine targets and the atlas, the capture frame''s open rule, the window crop, the budget and place count, the outcome merge and the facts.txt stage lines (ran / held / combined / declined / not armed / not reached)'
    Sources     = @('stage_dump_rules.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
