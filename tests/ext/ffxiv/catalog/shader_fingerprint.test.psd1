@{
    Class       = 'cpu'
    Note        = 'Structural shader fingerprint (chunks, ISGN/OSGN elements, SHEX version and length, stable 64-bit hash): golden hash, stability, every prefix and hostile field rejected with a PAGE_NOACCESS page behind the input, real D3DCompile+Strip shader, cost print'
    Sources     = @('shader_fingerprint.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Libs        = @('d3dcompiler.lib')
    Defines     = @('NOMINMAX')
}
