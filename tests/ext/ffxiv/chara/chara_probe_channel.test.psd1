@{
    Class       = 'cpu'
    Note        = 'CharaView probe command channel: the executor on a fake config and on the product CustomOptional behaviour (a command toggle is volatile: value_for_config stays unchanged, unknown lines ignored), the command file poll (100 ms age gate, 4 KiB limit, garbage, delete / rename / remember when it cannot be removed, unreadable file) and the 256 KiB result file (cap line, restart of a full file)'
    Sources     = @('chara_probe_channel.cpp')
    IncludeDirs = @('{repo}/tests/ext/hdr/stubs', '{repo}/OptiScaler', '{repo}/external/simpleini')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
