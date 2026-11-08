@{
    Class       = 'cpu'
    Note        = 'Engine pass override shader index (PassLayoutOfShader): the composite''s two names are found at the slots the compiled shader declares with its one output, another register layout follows the bytecode, a two-target buffer shader reports two outputs, the later knobs'' names are indexed, a geometry pass is not in the index, a texture named like the ambient buffer does not take its slot, and a truncated or foreign blob is refused instead of read'
    Sources     = @('engine_index.cpp', '{repo}/OptiScaler/ext/ffxiv/engine/PassIndex.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Libs        = @('d3dcompiler.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
