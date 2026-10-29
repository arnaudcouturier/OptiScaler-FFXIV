@{
    Class       = 'cpu'
    Note        = 'NR frame replay comparison: bit-exact compare over padded rows, first differing pixel, exact PSNR values, difference picture, bless request parsing, adapter check, provenance file'
    Sources     = @('nr_replay_compare.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
