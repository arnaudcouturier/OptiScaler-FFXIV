@{
    Class       = 'warp'
    Note        = 'Shadow mapping''s ambient entries in the ONE CbPatch dispatch on D3D11 WARP: the shipped ambient knob''s two entries and the shadows'' two appended into the same table are one Scale call''s dispatch (one per patched draw), the composed probe SH-recip field answers the multipliers'' product (base * 0.7 * 2.5), the header skyShRecipScale (byte 4, the field the shipped knob never touches) answers 2.5 alone, every other byte of a 12''352-byte table comes out bit for bit as it went in (the fixture seeds denormals, NaN payloads, integer-as-float bits and negative zero beside the addressed fields), the game''s own buffer is never written, the joining knob-at-1.0 arm runs two entries in the same shape, and a request past the end answers null with nothing dispatched'
    Prepare     = 'prepare_ambient.ps1'
    Sources     = @('shadows_ambient_warp.cpp', '{repo}/OptiScaler/ext/ffxiv/engine/CbPatch.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler', '{out}')
    Libs        = @('d3d11.lib', 'dxgi.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
