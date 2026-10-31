@{
    Class       = 'cpu'
    Note        = 'Contact shadow reflection index (LayoutOfShader): the five names of a sun light draw are found at the slots the compiled shader declares, another register layout follows the bytecode, the engine''s effect-style name table and two real blobs kept by the draw diagnostic''s fixture dump are read, a shader with the mask but no depth, the mask writer with no camera buffer and an ordinary geometry pass are all refused, and a truncated or foreign blob is refused instead of read'
    Sources     = @('contact_reflection.cpp', '{repo}/OptiScaler/ext/ffxiv/relight/ReflectionIndex.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Libs        = @('d3dcompiler.lib')
    Defines     = @('NOMINMAX')
    Args        = @('{repo}')
    Std         = 'c++latest'
}
