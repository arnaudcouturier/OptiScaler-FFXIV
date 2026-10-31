@{
    Class       = 'cpu'
    Note        = 'Live FG output switch observer: registration is idempotent and bounded to two, every observer is called with the step text, and each recorded step maps to its phase (a failed probation is a rollback)'
    Sources     = @('fg_switch_notify.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
