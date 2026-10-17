@{
    Class       = 'cpu'
    Note        = 'CharaView camera constant-buffer capture rules: accepted size window, the bound range (59 vectors, first constant), overflow safety, copy reuse'
    Sources     = @('chara_camera_rules.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
