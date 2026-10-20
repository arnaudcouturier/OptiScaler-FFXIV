@{
    Class       = 'cpu'
    Note        = '[REL] Source pins on the upstream release path (inputs/NVNGX_DLSS_Dx11.cpp) and its ext glue: the sleep before the reset is guarded by ShouldSkipReleaseSleep(drain) only, Failed refuses the release, the Dx11DelayedInit sleep after the reset is untouched, the seam reads the SkipReleaseSleep key and never passes shuttingDown=true'
    Sources     = @('release_upstream_pins_test.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('UNDEBUG')
}
