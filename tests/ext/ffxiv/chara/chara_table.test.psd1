@{
    Class       = 'cpu'
    Note        = 'CharaView table: accessor pattern scan on synthetic .text (0/1/2 hits, chunk straddling, RIP decoding), PE section lookup, fake-manager parse with the entry size and zeroing rules, lookup, 100 ms cache'
    Sources     = @('chara_table.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
