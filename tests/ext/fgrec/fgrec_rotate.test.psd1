@{
    Class       = 'cpu'
    Note        = 'FG recorder rotation and session cap: real Rotate shifts base to .1 to .2 with a continuation line and never a fourth file, and 16 MiB through the public path rotates once at the limit (a wrong threshold or lost rotation fails)'
    Sources     = @('fgrec_rotate.cpp')
    IncludeDirs = @('stubs', '.', '{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
