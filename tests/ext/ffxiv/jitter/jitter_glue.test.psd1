@{
    Class       = 'cpu'
    Note        = 'Jitter probe and correction core against a fake parameter block, fake process memory, clock and log: nothing runs with both keys off, every read precedes the write, no write unless every guard passed'
    Sources     = @('jitter_glue.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
