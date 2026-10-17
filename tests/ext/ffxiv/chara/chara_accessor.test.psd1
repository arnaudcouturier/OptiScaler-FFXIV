@{
    Class       = 'cpu'
    Note        = 'CharaView accessor resolution fails open: in a process that is not the matched game build the feature goes inert with exactly one log line, the table stays empty and nothing is read'
    Sources     = @('chara_accessor.cpp', '{repo}/OptiScaler/ext/ffxiv/chara/CharaTable.cpp', '{repo}/OptiScaler/ext/ffxiv/GameBuild.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
