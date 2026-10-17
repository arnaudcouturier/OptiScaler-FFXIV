@{
    Class       = 'cpu'
    Note        = 'CharaView shader tags: CRC lists (values, no overlap with each other or the UI set), role bits, private-data round trip on a fake pixel shader'
    Sources     = @('chara_tags.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Libs        = @('dxguid.lib')
    Defines     = @('NOMINMAX')
}
