@{
    Class       = 'cpu'
    Note        = 'S10 paste image ring model (ext/hudless/UiPasteRing.h): producer never reuses a busy slot, consumer picks the newest completed image <= 250 ms and never goes back in time, steady timing, invalidate, plus a randomised invariant run'
    Sources     = @('ui_paste_ring.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
