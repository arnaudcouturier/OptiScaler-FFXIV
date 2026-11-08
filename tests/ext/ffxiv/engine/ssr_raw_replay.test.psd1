@{
    Class       = 'local'
    Note        = 'Replay raw coherent SSR captures through the product Pass on WARP and require a byte-exact saved-output match.'
    Requires    = @('{env:OPTISCALER_SSR_RAW_DIR}/{env:OPTISCALER_SSR_RAW_FRAME}_scene.raw')
    Sources     = @('ssr_raw_replay.cpp', '{repo}/OptiScaler/ext/ffxiv/engine/SsrPass.cpp')
    IncludeDirs = @('stubs', '{out}', '{repo}/OptiScaler')
    Libs        = @('d3d11.lib')
    Defines     = @('NOMINMAX')
    Args        = @('{env:OPTISCALER_SSR_RAW_DIR}', '{env:OPTISCALER_SSR_RAW_FRAME}')
    Prepare     = 'prepare_ssr.ps1'
    Std         = 'c++latest'
}
