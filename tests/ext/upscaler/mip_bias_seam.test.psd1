@{
    Class       = 'cpu'
    Note        = 'Automatic mip bias wiring read from the production sources: one unconditional OnFeatureCreate call in ApplyForcedQuality, config keys bound, PostLoad hook, ini fragment'
    Sources     = @('mip_bias_seam_test.cpp')
    Args        = @('{repo}')
}
