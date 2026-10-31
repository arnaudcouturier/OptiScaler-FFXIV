@{
    Class       = 'cpu'
    Note        = 'The benchmark script and fault keys of the live output switch: the seconds:target parser, its dropped steps, the due/advance rule, and the named fault steps'
    Sources     = @('fg_output_switch_script.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
