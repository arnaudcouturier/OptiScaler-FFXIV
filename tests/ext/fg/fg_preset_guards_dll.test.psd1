@{
    Class       = 'manual'
    Note        = 'Runs the DLSS-G preset guards on a real nvngx_dlssg.dll: build tests\ext\fg\fg_preset_guards_dll.cpp with cl (include {repo}\OptiScaler, link bcrypt.lib), set OPTISCALER_DLSSG_DLL to the DLL, run the exe; asserts the RVAs found by hand for 310.9.1.0 and passes on any other DLL whose shape matches. Run it on a new runtime before trusting the live switch.'
    Sources     = @('fg_preset_guards_dll.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Libs        = @('bcrypt.lib')
    Std         = 'c++20'
}
