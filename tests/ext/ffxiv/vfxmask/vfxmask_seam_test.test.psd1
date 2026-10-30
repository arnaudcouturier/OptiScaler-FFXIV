@{
    Class       = 'cpu'
    Note        = 'VFX mask gate probe wiring, read from the sources: one Armed() load and one call in ext::dx11bridge::OnUpscaleBoundary, the // ext: B6 bridge seam still there and before the input copy, one mask-key write that is never a null pointer, the config key bound and off by default with its ini line (No menu control) and schema entry, and an R8_UNORM mask of the colour input''s size filled by the core rule'
    Sources     = @('vfxmask_seam_test.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
