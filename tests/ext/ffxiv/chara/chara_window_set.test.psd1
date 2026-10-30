@{
    Class       = 'cpu'
    Note        = 'CharaView processed window set: admission with hysteresis, the tie-breaks between candidates, the count range, the order table and the jitter gate, the measured render cadences, and the differential against the slot table with the set off'
    Sources     = @('chara_window_set.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
