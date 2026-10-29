@{
    Class       = 'local'
    Note        = 'E5 composition swapchain contract on WARP (needs a desktop session: hidden popups + DirectComposition): window helpers, no native HWND, DComp attach, resize keeps FLIP_SEQUENTIAL'
    Sources     = @('composition_smoke.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Libs        = @('d3d11.lib', 'dxgi.lib', 'dcomp.lib', 'user32.lib')
}
