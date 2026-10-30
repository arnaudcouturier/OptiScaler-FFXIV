@{
    Class       = 'cpu'
    Note        = 'Classic enlargement sharpening wiring pins: ini lines and defaults, load-time and dispatch clamps, skip rule, strength slot, one-shot pipeline, copy-back order and state restore, replay and shader wiring, menu grey rule'
    Sources     = @('nr_classic_sharpen_wiring.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
