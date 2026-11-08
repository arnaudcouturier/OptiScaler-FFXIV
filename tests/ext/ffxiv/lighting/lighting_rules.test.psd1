@{
    Class       = 'cpu'
    Note        = 'Lighting tab rules (ext/ffxiv/lighting/CharaAmbientRules.h): the shared name table''s g_AmbientParam row resolved by the rules themselves and distinct from the composite''s Array row, the lift clamp with the NaN-identity behaviour and its 0.5..2.0 range, the one scale entry addressing the block''s reciprocal-scale float4 (byte 48, .w the scalar) and nothing else, the 160-byte size test, the blend anchored at 1.0 (the master off, no reading yet, the lift at idle and a day frame all the engine''s own picture; night the key itself; between the thresholds the curve exactly the shadows module''s function), the per-frame cache (one patch per frame and buffer pointer, the epoch clearing it, the served-count expiry, the per-frame budget cap), the 64-refusal stand-down that a run resets, and the log lines a session reads'
    Sources     = @('lighting_rules.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
