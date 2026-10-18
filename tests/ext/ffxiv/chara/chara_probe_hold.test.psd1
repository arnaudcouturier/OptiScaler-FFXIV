@{
    Class       = 'warp'
    Note        = 'CharaView probe reference to a derived texture on WARP: an ordinary texture is held with exactly one reference, a swapchain buffer is recognised by DXGI_USAGE_BACK_BUFFER and never held, and ResizeBuffers fails while a buffer is referenced and works when it is not'
    Sources     = @('chara_probe_hold.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Libs        = @('d3d11.lib', 'dxgi.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
