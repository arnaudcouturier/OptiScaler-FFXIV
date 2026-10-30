@{
    Class       = 'cpu'
    Note        = 'FG recorder tap cost: 1000 key-off taps barely read the config and 1000 hot taps allocate nothing on the tap thread (a per-tap SyncKey or session-info rebuild fails)'
    Sources     = @('fgrec_tapcost.cpp', '{repo}/OptiScaler/ext/fgrec/FgRecorder.cpp')
    IncludeDirs = @('stubs', '.', '{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
