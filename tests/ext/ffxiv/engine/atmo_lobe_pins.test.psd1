@{
    Class       = 'cpu'
    Note        = 'Atmosphere''s sun lobe does not depend on the light shafts module (INT2): the stage owns its copy of the sky''s sun row (its own reader, held row, capture in its own seam entry, resets), includes no header of the shafts module and names none of its entry points or its key, the capture is not gated and no entry in front of the stage takes the sky''s sun draw, the read is in the one function every entry passes through and the disc is used only while fresh and agreeing; run on the rules with the shafts off (a capture that lands and then stops, one that never lands, one that returns after a gap, no light): the disc for exactly the freshness window and then the light''s direction bit for bit, never an old disc position'
    Sources     = @('atmo_lobe_pins.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
