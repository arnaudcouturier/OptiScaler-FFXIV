@{
    Class       = 'cpu'
    Note        = 'FG recorder mark key: one mark record per press, a = the number the log line shows, the count continues across a key-off/key-on and across sessions, and a press with the recorder off writes nothing (a broken counter or a duplicate record fails)'
    Sources     = @('fgrec_mark.cpp')
    IncludeDirs = @('stubs', '.', '{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
