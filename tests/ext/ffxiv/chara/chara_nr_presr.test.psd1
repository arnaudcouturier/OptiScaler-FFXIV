@{
    Class       = 'cpu'
    Note        = 'CharaView preview NR before SR (PreviewNRBeforeSR), pure rules: the placement is after SR for every input with the key off, latched or ineligible; a pre-SR run without an image leaves the evaluate on the plain colour and never starts the post-SR NR; a refused NR colour is evaluated again on the plain colour and latches NR before SR off once; tint and composed-from-NR follow the colour the evaluate used; shapes, result kinds, input checks, copy plan, latches, tallies and timer modes of the three placements; a placement flip rebuilds after the settle time'
    Sources     = @('chara_nr_presr.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
