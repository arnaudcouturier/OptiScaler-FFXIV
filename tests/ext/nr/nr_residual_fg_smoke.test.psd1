@{
    Class       = 'manual'
    Note        = 'NVIDIA GPU + installed nvngx.dll + official DLLs: residual NVIDIA FG (NGX FG parameter keys), static carriers and a translating plane; args = nvngx.dll, DLL directory; needs external/nvngx_dlss_sdk headers'
    Sources     = @('nr_residual_fg_smoke.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
}
