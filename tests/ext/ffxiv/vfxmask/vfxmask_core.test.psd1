@{
    Class       = 'cpu'
    Note        = 'VFX mask gate probe rules (ext/ffxiv/vfxmask/VfxMaskCore.h): the key values and case-insensitive parse, exact-complement left/right halves incl. odd widths, the 1.0/0.0 fill, the size guard, the log gate (one line per change, budget, re-arm) and the log text'
    Sources     = @('vfxmask_core.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
