@{
    Class       = 'cpu'
    Note        = 'Shadow mapping rules (ext/ffxiv/shadows/ShadowsRules.h): the key clamps with the NaN-identity behaviour, the PROVISIONAL light-luminance curve (full below 0.15, zero above 0.9, the smoothstep, the toggle that pins it, the no-reading identity), the square-compensated mask remap (identity at a lift of 0 over the whole R8 grid, the m^2 lift formula, the square root back and its round trip), the tent weights on a step edge, the two corrected ambient entries (the header field through float4[0].y, the probes through the engine''s AmbientSkyRecipEntry shape, both AddressFits against the two table sizes), the merged dispatch order (the knob''s entries first, the shadows'' appended, the composed field a product), the arming word, the 64-refusal stand-down and the log budget'
    Sources     = @('shadows_rules.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Std         = 'c++20'
}
