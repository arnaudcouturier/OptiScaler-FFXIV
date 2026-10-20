@{
    Class       = 'cpu'
    Note        = 'Bind-time mip bias numbers (ext/upscaler/MipBind.h): evaluate target, DLAA 0, size picks, eligibility, variant bias in add and replace'
    Sources     = @('mip_bind_math_test.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('UNDEBUG')
}
