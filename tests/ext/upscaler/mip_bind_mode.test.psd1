@{
    Class       = 'cpu'
    Note        = 'Add or replace detector (ext/upscaler/MipBind.h ModeDetector): the first-change rule, the 1/16 pair, constant art biases, restart on a second change'
    Sources     = @('mip_bind_mode_test.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('UNDEBUG')
}
