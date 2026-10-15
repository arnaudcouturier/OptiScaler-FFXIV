@{
    Class       = 'manual'
    Note        = 'NVIDIA GPU + installed nvngx.dll + official nvngx_dlss.dll directory: residual DLSS (private SR) through NGX; args = nvngx.dll, DLL directory; needs external/nvngx_dlss_sdk headers'
    Sources     = @('nr_residual_dlss_smoke.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
}
