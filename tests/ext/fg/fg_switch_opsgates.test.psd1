@{
    Class       = 'cpu'
    Note        = 'Live FG output switch present binding: the judgement gets the present duration and not its boot-relative start, a real stall still fails, and the probation fault fires once inside a probation only'
    Sources     = @('fg_switch_opsgates.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
