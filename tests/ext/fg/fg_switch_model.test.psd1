@{
    Class       = 'cpu'
    Note        = 'Live FG output switch model table: three consistent probe runs become M1/M2 or unknown, the witness answer, PE identities, the data file parsed against the compiled rows, and a lookup for an unmeasured build fails closed'
    Sources     = @('fg_switch_model.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}/tools/fg-probe/model-rows.txt')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
