@{
    Class       = 'cpu'
    Note        = 'Automatic mip bias glue (ext/upscaler/MipBias.h Apply): engage/publish/restore on a fake of CustomOptional, ini value kept aside, no write below 0.001, upstream Set/Reset while on'
    Sources     = @('mip_bias_glue_test.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('UNDEBUG')
}
