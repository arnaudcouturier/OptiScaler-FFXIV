@{
    Class       = 'cpu'
    Note        = 'Draw diagnostic: the DXBC RDEF reader names a shader compiled in the test (textures, a texture array, a sampler, constant buffers by slot) and refuses truncated or malformed blobs'
    Sources     = @('shader_bindings.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Libs        = @('d3dcompiler.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
