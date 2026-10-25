@{
    Class       = 'warp'
    Note        = 'Automatic ReShade depth, the capture glue of every NGX evaluate end to end on WARP with a fake NVSDK_NGX_Parameter and a fake host: the option off is one load of the gate word and nothing else, the character-preview Output publishes nothing and never reads the depth, render size per-axis minimum, output size fallbacks, the native device in the capture (NGX context only logged), option off and NGX shutdown from another thread carried out by the next evaluate, late registration, base never clamped, refusal and capture lines once per change, the raw draw only with the hooked context'
    Prepare     = 'prepare_depth_copy.ps1'
    Sources     = @('reshade_depth_eval.cpp', '{repo}/OptiScaler/ext/reshade/ReShadeDepthEval.cpp', '{repo}/OptiScaler/ext/reshade/ReShadeDepthCopy.cpp', '{repo}/OptiScaler/ext/reshade/ReShadeD3D11.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler', '{repo}/external/nvngx_dlss_sdk', '{out}/ext_gen')
    Libs        = @('d3d11.lib', 'dxgi.lib')
    Defines     = @('NOMINMAX', 'UNDEBUG', 'DEPTH_TEST_LOG')
    Std         = 'c++latest'
}
