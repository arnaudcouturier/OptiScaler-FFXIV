@{
    Class       = 'cpu'
    Note        = 'Bind-time mip bias settings and text (ext/upscaler/MipBind.h): default on for FFXIV, the AutomaticFFXIV alias, manual override wins, the observe log, the log and status lines'
    Sources     = @('mip_bind_settings_test.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('UNDEBUG')
    Std         = 'c++latest'
}
