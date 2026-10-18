@{
    Class       = 'cpu'
    Note        = 'Automatic mip bias numbers (ext/upscaler/MipBias.h): presets at 3840 incl. his -0.766061, output scaling, create sanity guard, offset clamp, hysteresis'
    Sources     = @('mip_bias_math_test.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('UNDEBUG')
}
