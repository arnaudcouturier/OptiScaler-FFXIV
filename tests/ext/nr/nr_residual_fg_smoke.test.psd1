@{
    Class       = 'local'
    Gpu         = $true
    Note        = 'NVIDIA GPU + installed nvngx.dll (OPTISCALER_NVNGX_DLL) + official nvngx_dlssg.dll folder (OPTISCALER_NGX_DLL_DIR): residual NVIDIA FG (NGX FG parameter keys), static carriers and a translating plane; needs external/nvngx_dlss_sdk headers'
    Requires    = @('OPTISCALER_NVNGX_DLL', 'OPTISCALER_NGX_DLL_DIR')
    Sources     = @('nr_residual_fg_smoke.cpp')
    IncludeDirs = @('{repo}/OptiScaler', '{repo}/external/nvngx_dlss_sdk')
    Libs        = @('d3d12.lib', 'dxgi.lib')
    Args        = @('{env:OPTISCALER_NVNGX_DLL}', '{env:OPTISCALER_NGX_DLL_DIR}')
}
