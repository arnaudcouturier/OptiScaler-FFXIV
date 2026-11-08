@{
    Class       = 'local'
    Note        = 'Diagnostic-only WARP replay of the same raw captures; saves red-channel ray exit codes as BGRA maps for pair comparison.'
    Requires    = @('{env:OPTISCALER_SSR_RAW_DIR}/{env:OPTISCALER_SSR_RAW_FRAME}_scene.raw')
    Sources     = @('ssr_raw_replay.cpp', '{repo}/OptiScaler/ext/ffxiv/engine/SsrPass.cpp')
    IncludeDirs = @('stubs', '{out}', '{repo}/OptiScaler')
    Libs        = @('d3d11.lib')
    Defines     = @('NOMINMAX')
    Args        = @('{env:OPTISCALER_SSR_RAW_DIR}', '{env:OPTISCALER_SSR_RAW_FRAME}', '{env:OPTISCALER_SSR_RAW_MAP}')
    Prepare     = 'prepare_ssr_raw_diag.ps1'
    Std         = 'c++latest'
}
