@{
    Class       = 'cpu'
    Note        = 'ReShade add-on registration state: the start-up state, what NoteRegistration (the late registration) records for every RegisterStatus, Registered(), and concurrent notes'
    Sources     = @('reshade_registration_state.cpp', '{repo}/OptiScaler/ext/reshade/ReShadeRegistrationState.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Defines     = @('NOMINMAX', 'UNDEBUG')
    Std         = 'c++latest'
}
