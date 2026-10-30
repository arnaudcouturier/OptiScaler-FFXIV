@{
    Class       = 'cpu'
    Note        = 'HDR census: the blend classification shared by the transmittance mask and the UI census equals the old BeforeDraw rule on all 115520 states; mask summary line text'
    Sources     = @('ui_blend.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Libs        = @('d3d11.lib', 'dxgi.lib', 'd3d12.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
