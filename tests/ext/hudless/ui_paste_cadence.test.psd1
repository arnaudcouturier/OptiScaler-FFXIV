@{
    Class       = 'cpu'
    Note        = 'Paste cadence counter (ext/hudless/UiPasteCadence.h): outputs per pasted image, image age, real frames never pasted, breaks of the paste, the log line and its window clock (10 s, ended by a pause)'
    Sources     = @('ui_paste_cadence.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
