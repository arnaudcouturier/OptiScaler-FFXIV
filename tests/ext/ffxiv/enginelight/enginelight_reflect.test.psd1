@{
    Class       = 'cpu'
    Note        = 'Engine light controls: the shader stamp read from the real bytecode of capture 20261107-164135 (tests/ext/ffxiv/enginelight/fixtures): the main moon shader is directional + fake specular at slot 2, the water pass''s copy is directional without fake specular, one of the ten cascade-atlas readers is nothing (no mask sampler), the water surface is the water at g_AmbientParam slot 3, a character shader that declares g_AmbientParam is not the water, junk is nothing'
    Sources     = @('enginelight_reflect.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
