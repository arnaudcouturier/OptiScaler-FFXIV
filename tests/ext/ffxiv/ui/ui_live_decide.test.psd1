@{
    Class       = 'cpu'
    Note        = 'S10-E: live source decisions without a GPU (acts only on R1 + scene stage + engine asked last Present, once per frame; render target guard size/format/samples/array; reason texts; arming rule; state and summary log lines)'
    Sources     = @('ui_live_decide.cpp', '{repo}/OptiScaler/ext/reshade/ReShadeBoundaryCore.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
