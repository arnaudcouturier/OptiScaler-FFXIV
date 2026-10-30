@{
    Class       = 'warp'
    Note        = 'HDR HUD halo: strips and properties P1-P10 on the CPU model, the frozen shader of main and the current Hdr10 shader (D3D11 WARP); legacy composition byte-for-byte'
    Prepare     = 'prepare_hdr_halo_shaders.ps1'
    Sources     = @('hdr_halo.cpp')
    IncludeDirs = @('{out}', '.')
    Libs        = @('d3d11.lib')
    Defines     = @('NOMINMAX', 'HALO_LAW')
    Std         = 'c++20'
}
