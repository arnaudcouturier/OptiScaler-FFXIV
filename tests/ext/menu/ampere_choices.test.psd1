@{
    Class       = 'cpu'
    Note        = 'RTX 20/30 Kernel image and Kernel family combos: the choice shown for a stored value equals what the runtime settings writer (AmpereMfgLoader.h FormatIniContent030, ResolveRouter) does with it, case variants and padded spellings included'
    Sources     = @('ampere_choices.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
