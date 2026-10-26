@{
    Class       = 'cpu'
    Note        = 'product registry soundness (ids, bindings, group files independent), FieldTable.h vs the members of GameBuild.h (drift test reads the header), field accessors, empty-image resolve'
    Sources     = @('resolve_registry.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Args        = @('{repo}')
}
