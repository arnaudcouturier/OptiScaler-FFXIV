@{
    Class       = 'warp'
    Note        = 'Plate game side on WARP: every nameplate draw condition is a real D3D11 state, a broken one keeps the draw native and asks the engine nothing, one answer per frame, a hidden frame draws only into the plate image, a shadow frame draws both places, and a hidden frame whose redirect cannot run draws natively'
    Sources     = @('plate_game_frame.cpp', '{repo}/OptiScaler/ext/ffxiv/plates/PlateNative.cpp')
    IncludeDirs = @('stubs', 'engine_stubs', '{repo}/OptiScaler')
    Libs        = @('d3d11.lib', 'dxgi.lib', 'd3dcompiler.lib')
    Defines     = @('NOMINMAX', 'UNDEBUG')
    Std         = 'c++20'
}
