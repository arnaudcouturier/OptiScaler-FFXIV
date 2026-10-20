@{
    Class       = 'cpu'
    Note        = 'Supported FFXIV build identity and every game address, structure offset and byte guard pinned to the values found in the 2026.10.16 executable; old build and near misses do not match'
    Sources     = @('game_build.cpp', '{repo}/OptiScaler/ext/ffxiv/GameBuild.cpp')
    IncludeDirs = @('../chara/stubs', '{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
