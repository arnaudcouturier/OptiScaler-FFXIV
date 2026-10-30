@{
    Class       = 'warp'
    Note        = 'Native nameplates end to end on WARP: the product game side and the product engine compiled together, frames drawn through the real classifier, the present seam and the output pass; steady state, a refused draw publishes no stale image, a UI draw inside a hidden frame stays native, and a frame with no plate draw takes the image away'
    Prepare     = '../../hudless/prepare_ui_shaders.ps1'
    Sources     = @('plate_scenarios.cpp', '{repo}/OptiScaler/ext/hudless/UiPlates.cpp', '{repo}/OptiScaler/ext/hudless/SharedSlot.cpp', '{repo}/OptiScaler/ext/ffxiv/plates/PlateNative.cpp')
    IncludeDirs = @('{repo}/tests/ext/hudless/plate_stubs', '{repo}/tests/ext/hudless/ui_stubs', '{repo}/tests/ext/gpu/stubs', '{repo}/tests/ext/ffxiv/plates', '{out}', '{repo}/OptiScaler')
    Libs        = @('d3d11.lib', 'd3d12.lib', 'dxgi.lib', 'd3dcompiler.lib', 'ole32.lib')
    Defines     = @('PLATES_CHECKED_MUTEX', 'PLATES_GAME_SIDE', 'NOMINMAX', 'PLATES_TEST_CLOCK', 'PLATES_NOW=plate_test_clock::Now')
    Std         = 'c++20'
}
