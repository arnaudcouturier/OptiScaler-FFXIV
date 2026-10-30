@{
    Class       = 'cpu'
    Note        = 'FG recorder files: long directories still record, sweep plus start keeps 5 sessions, locked rotation fails loud, names carry millis'
    Sources     = @('fgrec_files.cpp')
    IncludeDirs = @('stubs', '.', '{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
