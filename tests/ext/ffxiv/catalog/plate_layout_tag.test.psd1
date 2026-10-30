@{
    Class       = 'cpu'
    Note        = 'Byte widths of the nameplate input-layout element formats (every format the layout tagging handles, with its width)'
    Sources     = @('plate_layout_tag.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
