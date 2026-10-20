@{
    Class       = 'cpu'
    Note        = 'Bind-time mip bias wiring read from the production sources: the three seam calls, config keys and ini fragment, no lock and no Config in the engine, the older module standing down, no upstream file touched'
    Sources     = @('mip_bind_seam_test.cpp')
    Args        = @('{repo}')
    Std         = 'c++latest'
}
