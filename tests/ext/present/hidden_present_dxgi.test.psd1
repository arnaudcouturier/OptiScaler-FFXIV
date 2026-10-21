@{
    Class       = 'warp'
    Note        = '[PACE] DO_NOT_WAIT is accepted by a real DXGI swapchain (WARP, hidden window; flip-discard, flip-sequential, discard, sequential): only success or WAS_STILL_DRAWING comes back'
    Sources     = @('hidden_present_dxgi_test.cpp')
    Libs        = @('d3d11.lib', 'dxgi.lib', 'user32.lib')
}
