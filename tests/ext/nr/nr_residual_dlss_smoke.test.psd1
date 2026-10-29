@{
    Class       = 'local'
    Gpu         = $true
    Note        = 'NVIDIA GPU + installed nvngx.dll (OPTISCALER_NVNGX_DLL) + official nvngx_dlss.dll folder (OPTISCALER_NGX_DLL_DIR): residual DLSS (private SR) through NGX; needs external/nvngx_dlss_sdk headers'
    Requires    = @('OPTISCALER_NVNGX_DLL', 'OPTISCALER_NGX_DLL_DIR')
    Sources     = @('nr_residual_dlss_smoke.cpp')
    IncludeDirs = @('{repo}/OptiScaler', '{repo}/external/nvngx_dlss_sdk')
    Libs        = @('d3d12.lib', 'dxgi.lib')
    Args        = @('{env:OPTISCALER_NVNGX_DLL}', '{env:OPTISCALER_NGX_DLL_DIR}')
}
