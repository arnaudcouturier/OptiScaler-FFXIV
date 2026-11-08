@{
    Class       = 'cpu'
    Note        = 'CharaView passes that draw from inside a hooked game draw (spatial AA, DLSS guide passes) save and restore the state they touch explicitly instead of swapping into a private device-context state: the guard with its Spec (AA 1/1/0, guides 4/1/2), Neutralise() and no explicit HS/DS/GS nulls, the rasterizer, blend and depth-stencil defaults the guide scope now sets itself, the order of the calls around the guard, and a full snapshot around every NGX CreateFeature / EvaluateFeature on the game context'
    Sources     = @('chara_state_pins.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
