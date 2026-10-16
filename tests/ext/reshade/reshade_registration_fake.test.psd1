@{
    Class   = 'cpu'
    Note    = 'S10 D0: RegisterAddon + register_event against a stand-in ReShade DLL exporting the add-on API (ok and refuse modes)'
    Sources = @('reshade_registration_fake.cpp', 'fake_reshade.cpp')
    Runner  = 'run_reshade_registration_fake.ps1'
    Std     = 'c++latest'
}
