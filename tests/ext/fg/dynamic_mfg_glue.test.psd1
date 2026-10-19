@{
    Class       = 'cpu'
    Note        = 'Dynamic MFG glue (ext/fg/DynamicMfgGlue.h) on a fake world: slider kept under the selector, recovery once, Default hands off, Ada clamp for Streamline only, closed-gate latch, fail open'
    Sources     = @('dynamic_mfg_glue.cpp')
    IncludeDirs = @('{out}', '{repo}/OptiScaler', '{repo}/external/streamline')
    Prepare     = 'prepare_dynamic_mfg_config.ps1'
    Defines     = @('NOMINMAX')
}
