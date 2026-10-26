@{
    Class       = 'cpu'
    Note        = 'CharaView as the first full resolver seam on committed bytes: pattern equals CharaViewBuild, current-build layout agrees with the table and the CharaTable scan, previous-build (B, B-read rows only, maskB honoured) finds 0x26ab40 / 0x28f9490, wrong-build and decoy controls'
    Sources     = @('resolve_chara_b_fixture.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
