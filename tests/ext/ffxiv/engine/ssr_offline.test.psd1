@{
    Class       = 'local'
    Note        = 'Screen-space reflections offline lab (OPTISCALER_SSR_OFFLINE_DIR = one capture folder, OPTISCALER_SSR_OFFLINE_OUT = an output folder): runs the product pass as it ships on the capture''s own view position, scene colour, camera table and engine planar map on the WARP adapter and writes the map (out.bin), the raw term (term.bin) and the hit-distance guide (guide.bin) for analysis. Local only: skipped when the folders are not named.'
    Requires    = @('OPTISCALER_SSR_OFFLINE_DIR', 'OPTISCALER_SSR_OFFLINE_OUT')
    Sources     = @('ssr_offline.cpp', '{repo}/OptiScaler/ext/ffxiv/engine/SsrPass.cpp')
    IncludeDirs = @('stubs', '{out}', '{repo}/OptiScaler')
    Libs        = @('d3d11.lib')
    Defines     = @('NOMINMAX')
    Args        = @('{env:OPTISCALER_SSR_OFFLINE_DIR}', '{env:OPTISCALER_SSR_OFFLINE_OUT}')
    Prepare     = 'prepare_ssr.ps1'
    Std         = 'c++latest'
}
