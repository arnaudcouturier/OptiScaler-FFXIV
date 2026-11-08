@{
    Class       = 'local'
    Note        = 'Offline WARP SSR phase A/B: save normal color output from original or isolated half-pixel shader variant.'
    Requires    = @('{env:OPTISCALER_SSR_RAW_DIR}/{env:OPTISCALER_SSR_RAW_FRAME}_scene.raw')
    Sources     = @('ssr_raw_replay.cpp', '{repo}/OptiScaler/ext/ffxiv/engine/SsrPass.cpp')
    IncludeDirs = @('stubs', '{out}', '{repo}/OptiScaler')
    Libs        = @('d3d11.lib')
    Defines     = @('NOMINMAX')
    Args        = @('{env:OPTISCALER_SSR_RAW_DIR}', '{env:OPTISCALER_SSR_RAW_FRAME}', 'save', '{env:OPTISCALER_SSR_PHASE_OUTPUT}')
    Prepare     = 'prepare_ssr_phase_ab.ps1'
    Std         = 'c++latest'
}
