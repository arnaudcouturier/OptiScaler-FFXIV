@{
    Class       = 'cpu'
    Sources     = @('nr_lighting_history.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
