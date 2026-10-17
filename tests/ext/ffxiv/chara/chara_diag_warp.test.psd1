@{
    Class       = 'warp'
    Note        = 'CharaView DLSS diagnostics on D3D11 WARP: off unless PreviewGuideCapture is on; six same-frame images plus the camera vector are staged, read back without waiting and written with the 12 byte header; the second dump needs more than 120 evaluations; 12 files at most; trace lines need PreviewNRTrace and are rate limited'
    Sources     = @('chara_diag_warp.cpp', '{repo}/OptiScaler/ext/ffxiv/chara/CharaDiag.cpp',
                    '{repo}/OptiScaler/ext/core/ExtConfig.cpp', '{repo}/OptiScaler/ext/hdr/HdrConfig.cpp',
                    '{repo}/OptiScaler/ext/hudless/HudlessConfig.cpp', '{repo}/OptiScaler/ext/hudless/HudlessState.cpp')
    IncludeDirs = @('stubs', '{repo}/tests/ext/hdr/stubs', '{repo}/OptiScaler', '{repo}/external/simpleini')
    Libs        = @('d3d11.lib', 'dxgi.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
