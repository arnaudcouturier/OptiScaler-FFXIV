@{
    Class       = 'cpu'
    Note        = 'HDR converter constants: legacy composition by default, UI expansion resolution and snapshot, paper-white fallback without a scene, shared clamps, preview-mask barrier decision'
    Sources     = @('hdr_constants.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Std         = 'c++20'
}
