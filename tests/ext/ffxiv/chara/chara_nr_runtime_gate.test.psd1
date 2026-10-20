@{
    Class       = 'cpu'
    Note        = 'Preview NR follows the NR runtime selection THROUGH the closed gate: a model failure latch and a "no model" probe close the draw gate, so the revision is observed from maintenance too (production observer + real availability and plumbing rules; the old wiring is shown stuck)'
    Sources     = @('chara_nr_runtime_gate.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
