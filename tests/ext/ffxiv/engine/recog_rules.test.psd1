@{
    Class       = 'cpu'
    Note        = 'Recognition and frame-step rules on the host, from rows copied out of four live captures: depth of field''s camera band accepts the real reversed-Z inverse projection (near 0.1 m, no far plane, 10 m at depth 0.01) and keeps refusing every negative or degenerate row, the light bounce''s camera checks keep accepting the four jittered rows, the frame counter moves once for the composite''s two adjacent draws and a declining frame still closes, the fog height profile is chosen by the stamped slot with the 48-byte check refusing the clip-to-world stand-in, and the pixel-to-NDC mapping derived from the render window reproduces the sun pixel the game reported'
    Sources     = @('recog_rules.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
