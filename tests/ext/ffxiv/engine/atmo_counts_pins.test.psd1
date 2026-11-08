@{
    Class       = 'cpu'
    Note        = 'Atmosphere''s timed counts line, wiring pins read as text (INT2): the three entries reach the pass through StageAtToneMap whose first statements tick the line and count the entry, the three call sites pass three different entries, declines and skips are counted per reason where they are noted, the line is taken in one place from the timed rules on the wall clock and the old rendered-frame cadence is gone, the output-frame heartbeat ticks the line and the engine pass reaches it while the stage is active, the counts are reset with the stage''s other counters, and the stage names its entries by hook never by caller'
    Sources     = @('atmo_counts_pins.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
