@{
    Class       = 'cpu'
    Note        = 'CharaView private context state: the flags follow the threading mode of the game device, the same for the spatial AA and the DLSS guides'
    Sources     = @('chara_context_state.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
