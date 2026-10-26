@{
    Class       = 'cpu'
    Note        = 'Companion reads ActiveBuild(): the table before the startup publishes, then a healed batch format offset is followed and the module / layout RVAs come from the active build'
    Sources     = @('companion_active_build_test.cpp')
    Args        = @('healed')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
