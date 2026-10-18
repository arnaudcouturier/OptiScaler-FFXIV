@{
    Class       = 'cpu'
    Note        = 'CharaView teardown at unhook: release at once only on the draw thread, otherwise a request the draw thread performs at its next entry (exactly once, never on the unhook thread); the dispatcher gate stays open while it is pending'
    Sources     = @('chara_lifecycle.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
