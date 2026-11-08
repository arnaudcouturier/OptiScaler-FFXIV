@{
    Class       = 'cpu'
    Note        = 'God rays volumetric march, pure rules: the fog decode''s byte-to-field table against the offsets the engine''s own fog post reads (every field of cFogParam and cExpHeightFogParam, a null half answering a state the gate refuses), the density profile''s identity with the engine''s own formula as the disassembly states it (the 0.545968 literal, the two-exp shape, the height lift at 0 and 2), the phase exponent''s clamp to the engine''s own floor of 1, the march far''s band and its nothing-readable answer, the cascade order (finest footprint first, unusable cascades left out), the atlas texel size and the disc scale the writer''s own constants produce, the shadowed compare''s no-light-on-a-broken-read rule, the edge fades and the distance ramp reproducing the captured instances'' band edges, the knob clamps (NaN to default, the ranges), the march extent for quarter and half, the term clamp, the engine fog colour rule (the LUT u, the sky lerp, the sky pixel''s zeroed weight), the outcome and decline names (the hold stands vs the engine''s own wordings), the skip decomposition counting once, which declines count toward the stand-down, the 64-refusal stand-down on the march''s own knob, and every default key building today''s picture'
    Sources     = @('godrays_rules.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
