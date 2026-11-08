@{
    Class       = 'cpu'
    Note        = 'Effect meter (METER) pure rules on the host: the table (one row per stage, the channels and regions each stage is measured on), the measure (display levels of the stored value over the frame''s white, the strict 8/255 threshold, the largest channel, NaN as nothing), the five region kinds at their thresholds, the sampling geometry (the smallest block that fits, strata, seeds, exact for small boxes), tap totals within 1 % of the full diff and equal on an exact geometry, key off = exactly 0, the read-back reduction, the schedule (one effect per sampled frame, three seconds apart, a lost result dropped), the per-frame book (ran beats skipped, reasons keyed with numbers folded, stale tokens ignored, window into total), the log clocks (10 s, 60 s, 5 min) and the wording of the summary line, the card and the effect line'
    Sources     = @('meter_rules.cpp')
    IncludeDirs = @('../engine', '{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
