@{
    Class       = 'cpu'
    Note        = 'CharaView preview NR recipe: Encode (pass-through) -> model -> Resolve recorded against a fake that tracks every image state (shared images rest in COMMON, private ones in UAV, no image read and written in one pass), the constants of each pass, the model call arguments, the zero-motion pass on the first run only'
    Sources     = @('chara_nr_recipe.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
