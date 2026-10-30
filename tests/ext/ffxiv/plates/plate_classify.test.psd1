@{
    Class       = 'cpu'
    Note        = 'The plate-draw rule (the game-side classifier): the perfect plate state passes, a zeroed state refuses, and flipping any one of the 14 conditions refuses, so a wrong condition keeps the plates native'
    Sources     = @('plate_classify.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
