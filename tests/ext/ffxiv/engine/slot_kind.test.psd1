@{
    Class       = 'cpu'
    Note        = 'One typed binding lookup for every module (diag::BindPointOf): a fixture compiled with explicit and different register(sN)/register(tM) values lists its samplers before its textures as the game''s compiler does, the effect compiler''s .T/.S names are then put on the compiled entries, and every module''s slot function answers the TEXTURE''s register - the reflections (whose name-only scan answered the sampler''s), the ambient occlusion, the ground wetness, the shader index and the contact shadow index - while a buffer name asked for as a texture is nothing, a name the shader does not carry is the missing answer rather than slot 0, a compiled table without the suffix answers the same slots, and a text pin holds that no module compares a binding''s type by hand any more'
    Sources     = @('slot_kind.cpp',
                    '{repo}/OptiScaler/ext/ffxiv/engine/PassIndex.cpp',
                    '{repo}/OptiScaler/ext/ffxiv/relight/ReflectionIndex.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Libs        = @('d3dcompiler.lib')
    Defines     = @('NOMINMAX')
    Args        = @('{repo}')
    Std         = 'c++latest'
}
