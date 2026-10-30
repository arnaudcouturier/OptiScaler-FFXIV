@{
    Class       = 'cpu'
    Note        = 'FG recorder no-paths rule: the exe arrives as a file name, session.begin carries no folder or directory field, the module strip keeps file names only, and a session file holds no drive letter, Users segment or recorded directory (a leaking record fails)'
    Sources     = @('fgrec_nopaths.cpp')
    IncludeDirs = @('stubs', '.', '{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
