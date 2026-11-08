@{
    Class       = 'cpu'
    Note        = 'Recognition and frame-step wiring pins, read as text from the sources: depth of field, god rays and the light bounce open their count through FrameStep at the composite and never advance it themselves, the frame closes at the top of all three tone-map entries before the one-answer gate and only through ClosesFrame (a replay never closes it), each stage writes its step line once a state, the fog height profile is picked by the stamped slot with the stamp written inside the creation hook''s try before the layout parse and the declared 48 bytes checked before the decode, the pixel-to-NDC mapping is derived from the window and the viewport row only logged, and every new wait has a line of its own'
    Sources     = @('recog_pins.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
