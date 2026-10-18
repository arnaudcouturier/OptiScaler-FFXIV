@{
    Class       = 'cpu'
    Note        = 'CharaView routing: a DLSS path that can run wins; otherwise the spatial AA takes the equipment display draw, also when PreviewDLSS is on but its path cannot run, whatever the draw API (the game issues it as DrawIndexed); everything else stays native'
    Sources     = @('chara_route.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
