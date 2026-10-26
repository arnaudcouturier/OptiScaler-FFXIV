@{
    Class       = 'cpu'
    Note        = 'Jitter source constants: the strength floor and the NDC y sign mask come from the build (GameBuild.h) instead of literals: the build holds the old values, the strength follows a different floor, a contradicting sign mask is refused instead of projected wrongly, the observe detour carries the mask'
    Sources     = @('jitter_source_constants.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
