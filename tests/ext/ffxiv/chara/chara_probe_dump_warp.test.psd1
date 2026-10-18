@{
    Class       = 'warp'
    Note        = 'CharaView probe image dump on D3D11 WARP: staging copies with an event query and a no-wait map, mip 0 of a texture with mips, snapshot at copy time, PNG files identical to the encoder output, one dump in flight, 10 s give-up, absent/unsupported/multisampled images become state.txt lines, 40 file session budget refuses the ninth dump with nothing written'
    Sources     = @('chara_probe_dump_warp.cpp', '{repo}/OptiScaler/ext/ffxiv/chara/CharaProbeDump.cpp')
    IncludeDirs = @('stubs', '{repo}/tests/ext/hdr/stubs', '{repo}/OptiScaler', '{repo}/external/simpleini')
    Libs        = @('d3d11.lib', 'dxgi.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
