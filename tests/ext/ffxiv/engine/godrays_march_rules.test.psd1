@{
    Class       = 'cpu'
    Note        = 'God rays, beams that follow the engine''s own shadows, pure rules on the host: the new keys (GodraysDensity 20, GodraysAnisotropy 0.8, 48 steps, composite gain 0.7) with the first march''s calls still building, the Henyey-Greenstein lobe (1 at the light, 0.1 at 24.6 degrees for g 0.8, monotone, the phase-boost narrowing), the in-scatter integral against its closed discrete form (extinction accruing through shadow, the no-extinction mean as the mutant it separates), a cascade''s atlas slice read from its own matrix and fade row on all sixteen matrices of the four rc6 frames (a set of three keeps its slices, unreadable rows have none), the sky ceiling from a 128-bin histogram (interpolated 99th percentile, 0.98 margin, no sky samples no ceiling, the clamp and its property), the light gates (luminance gain 0 at 0.05 and 1 at 0.6, the nearest pixel ray at 8.3 / 65.7 / 0 / 117.5 degrees, the aside gate under cosine 0.3 with synthetic lights at 80 and 70 degrees, the cascade axis to 0.05 degrees and its 3-degree limit), the one verdict function with its wordings, the fog sun decoded from the 128-byte copy without the profile, the stage values string, and the stats texture layout'
    Sources     = @('godrays_march_rules.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
