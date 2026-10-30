@{
    Class       = 'warp'
    Note        = 'VFX mask gate probe write path (ext/ffxiv/vfxmask/VfxMaskApply.h) against a fake NVSDK_NGX_Parameter and a real WARP mask: the DLSS bias current colour mask key is written once per evaluate with the mask pointer only while the probe is armed, nothing at all is written with the key off (the block is not even read) or when the mask cannot be used, a block that does not keep the pointer reports unconfirmed, and switching off after a run writes nothing more'
    Sources     = @('vfxmask_apply_warp.cpp', '{repo}/OptiScaler/ext/ffxiv/vfxmask/VfxMaskTexture.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler', '{repo}/external/nvngx_dlss_sdk')
    Libs        = @('d3d11.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
