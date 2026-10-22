@{
    Class       = 'cpu'
    Note        = 'Jitter source fix instruction guards: the table equals the bytes read from the supported executable, the call targets / rip-relative globals / structure offsets are the ones the bytes encode, any changed or unreadable byte or a moved image is refused and names its block'
    Sources     = @('jitter_source_anchors.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
