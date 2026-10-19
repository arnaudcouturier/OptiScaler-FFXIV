@{
    Class       = 'cpu'
    Note        = 'Renamed menu labels of the fork cards as source-text pins (wanted strings present, old strings gone), each row also run against a broken copy: Forced quality level, the RTX 20/30 frame-generation runtime wording, Target FPS labels, Show detected UI (HUD-less)'
    Sources     = @('labels_text.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
