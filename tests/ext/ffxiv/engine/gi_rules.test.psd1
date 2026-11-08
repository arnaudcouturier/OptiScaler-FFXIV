@{
    Class       = 'cpu'
    Note        = 'Screen-space light bounce pure rules: the keys'' clamps and NaN answers, the tangent basis''s orthonormality and the hemisphere''s mean cosine 2/3, the per-pixel hash''s stability and the salt, the hit''s reflected radiance (the stored scene value squared, the surface''s colour left in, no albedo quotient; the clamp at 8 linear refuses a runaway and passes a lamp under it), the store''s data marker (the rays traced: a traced texel with no hit is a valid zero, an untraceable origin has none), the strength default 0.55 and the gain 1/pi by value, the origin gate''s special-material bytes and selector and diffuse weight, the estimator''s constant-radiance hemisphere integral and the miss''s nothing and no pair renormalization, the view fade''s camera cosine, the smooth stencil''s 25 integer taps (the full 5x5 square, the direct neighbours included), its 1-4-6-4-1 outer-product weights (sum 256, symmetric, falling with the distance) and its exactly zero response at the checkerboard frequency and the one-texel stripes, the compose''s strength scaling and the NaN key''s default, the outcome wordings and the stand-down''s counted reasons, the card''s decomposition and the run and cost lines'
    Sources     = @('gi_rules.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
