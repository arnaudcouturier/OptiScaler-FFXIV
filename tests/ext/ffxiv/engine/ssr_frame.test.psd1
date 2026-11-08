@{
    Class       = 'local'
    Note        = 'Screen-space reflections offline (OPTISCALER_SSR_FRAME_DIR = one capture folder): loads the dump and the camera table of the capture''s own water draw, runs the product''s reflection pass on the WARP adapter with the march compiled for its exit codes (prepare_ssr_frame.ps1), and prints the share of texels that found a reflection and the histogram of why the others did not; with no view position and no depth target in the dump it says exactly what is missing and measures a reference frame built from the same camera table instead. Local only: skipped when the folder is not named.'
    Requires    = @('OPTISCALER_SSR_FRAME_DIR')
    Sources     = @('ssr_frame.cpp', '{repo}/OptiScaler/ext/ffxiv/engine/SsrPass.cpp')
    IncludeDirs = @('stubs', '{out}', '{repo}/OptiScaler')
    Libs        = @('d3d11.lib')
    Defines     = @('NOMINMAX')
    Args        = @('{env:OPTISCALER_SSR_FRAME_DIR}')
    Prepare     = 'prepare_ssr_frame.ps1'
    Std         = 'c++latest'
}
