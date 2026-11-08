@{
    Class       = 'cpu'
    Note        = 'Engine light controls (ENG2) pure rules on the host: the contract''s defaults and the ranges (NaN -> the default), Wanted (a lamps-only or cb-only configuration is wanted: the feed is not tied to the exposure stage), the effective factor (master off, the A/B and weight 0 are exactly 1), the wall-clock weight (63 % in a second at any update rate, the step cap, the snap) and the shared night curve on every capture on disk, the exposure fingerprint of the real g_CommonParameter bytes, the compensation formula s_hold / s_now with its bounds and identities, and the controller on a synthetic first-order-lag engine: a day -> night step latches the settled identity exposure and never the day one, the hold does not follow s while the controls act, the A/B release re-latches and a short A/B does not, a gap forgets the hold, the wait runs out into a logged latch, no reading fails open, the forced weight never waits, a dusk never uses a day latch'
    Sources     = @('engine_light_rules.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
