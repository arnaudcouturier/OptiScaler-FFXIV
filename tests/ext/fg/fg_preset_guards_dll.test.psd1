@{
    Class       = 'manual'
    Note        = 'Runs the DLSS-G preset guards on a real nvngx_dlssg.dll: build tests\ext\fg\fg_preset_guards_dll.cpp with cl (/std:c++20 /EHsc /DNOMINMAX, include {repo}\OptiScaler, link bcrypt.lib), set OPTISCALER_DLSSG_DLL to the DLL, run the exe. Prints every located RVA, where create keeps and reloads its context argument and where the Ada unlock writes; asserts the RVAs read with a decompiler for 310.9.16, 310.9.1.0 and 310.8.0 and passes on any other DLL whose code shape the guards accept. Run it on a new runtime before trusting the live switch.'
    Sources     = @('fg_preset_guards_dll.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Libs        = @('bcrypt.lib')
    Std         = 'c++20'
}
