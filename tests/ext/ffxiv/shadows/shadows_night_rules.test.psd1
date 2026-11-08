@{
    Class       = 'cpu'
    Note        = 'Shadow mapping night curve on the visible sun (ShadowsRules.h): smoothstep(sin(-1 deg), sin(-7 deg)) of the fog sun''s world y gives 0 / 0.958 / 0.335 / 0 on the four rc6 frames (the colour curve said 1 on the daylight frame of capture 4), the thresholds are the two sines, a fresh fog sun wins over the light colour both ways and a stale or absent one falls back to the colour (neither gives the identity, the toggle pins full effect), the freshness bound on the consumer''s rendered-frame clock is the same 120 frames at 1x and 4x present-counter stepping, the mask lift and the ambient table both read the one curve, the route wants the fog sun only when the curve can act (off by default), and the once-per-change source lines'
    Sources     = @('shadows_night_rules.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Std         = 'c++20'
}
