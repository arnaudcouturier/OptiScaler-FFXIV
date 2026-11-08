@{
    Class       = 'warp'
    Note        = 'Draw diagnostic over a real draw: a flare shader created while the capture waits, with the session reflection table full, is named from the shader kept from creation (its two buffers sampled into cbdump.csv), engine.txt''s sun line is projected from the diagnostic''s own light read with no light shafts module in the build, and the reflections'' last water draw is this capture''s only'
    Sources     = @('draw_diag_capture.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Libs        = @('d3d11.lib', 'd3dcompiler.lib', 'user32.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
