@{
    Class       = 'cpu'
    Note        = 'CharaView routing: PreviewDLSS wins, spatial AA takes the equipment display draw whatever the draw API (the game issues it as DrawIndexed), everything else stays native'
    Sources     = @('chara_route.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
