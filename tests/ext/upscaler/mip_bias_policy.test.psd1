@{
    Class       = 'cpu'
    Note        = 'Automatic mip bias source policy (ext/upscaler/MipBias.h): follow the latest accepted create; replay of the logged session, ignored creates, offset, auto off/on, log lines'
    Sources     = @('mip_bias_policy_test.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('UNDEBUG')
}
