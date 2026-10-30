@{
    Class       = 'cpu'
    Note        = 'FG recorder seams: every tap writes its kind and fields through the recorder with stand-ins (mapping units, begin/end pairing with durations and willPresent gating, provider generation bumps, module file names, whole-file no-paths scan)'
    Sources     = @('fgrec_seams.cpp', '{repo}/OptiScaler/ext/fgrec/FgRecorder.cpp')
    IncludeDirs = @('stubs', '.', '{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
