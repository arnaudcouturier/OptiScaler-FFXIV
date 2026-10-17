@{
    Class       = 'cpu'
    Note        = 'Help / Status rows for the DLSS DLLs (ext/upscaler/DlssDlls.h): first copy in NGX search order is "in use", later copies "not used"; snippet version text; unloaded copy'
    Sources     = @('dlss_dll_rows.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('UNDEBUG')
}
