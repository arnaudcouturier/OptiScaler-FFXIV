@{
    Class       = 'cpu'
    Note        = 'DLSS-G preset driver-settings seam against the real NVAPI types: NOT_FOUND becomes OK only for an explicit request, other errors and non-DWORD replies untouched, Auto untouched, unavailable control does not answer'
    Sources     = @('fg_preset_drs.cpp', '{repo}/OptiScaler/ext/fg/dlssg/PresetDrs.cpp')
    IncludeDirs = @('drs_stubs', '{repo}/OptiScaler', '{repo}/external/nvapi')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
