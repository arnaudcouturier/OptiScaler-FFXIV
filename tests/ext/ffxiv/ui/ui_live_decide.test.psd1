@{
    Class       = 'cpu'
    Note        = 'S10-E: live source decisions without a GPU (acts only on R1 + scene stage + engine asked last Present, once per frame; render target guard size/format/samples/array; reason texts; arming rule; state and summary log lines; AnchorRule = early: when it acts, the ReShade gate on the predicted result, the R1 off-screen event mapping, the order of its fallbacks before any context call, the rule change line, the early anchors field, the decline counters by cause and their summary detail, absent with r1b)'
    Sources     = @('ui_live_decide.cpp', '{repo}/OptiScaler/ext/reshade/ReShadeBoundaryCore.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
