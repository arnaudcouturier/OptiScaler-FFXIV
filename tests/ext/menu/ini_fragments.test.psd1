@{
    Class       = 'cpu'
    Note        = 'Ini hygiene: every config key the fork binds (b.Add literals under ext, comments stripped) has a commented line in the ini fragment named after its section; the keys without a menu control on purpose say "No menu control" (negative controls for each rule; the generated NR Pass keys are outside the rule)'
    Sources     = @('ini_fragments.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
