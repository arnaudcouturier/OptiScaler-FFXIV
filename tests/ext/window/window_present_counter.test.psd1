@{
    Class       = 'cpu'
    Note        = 'Window mode M2 on fakes: a present that never reached the frame generation present is not counted by the off gate (bridge present counter unchanged); wrap, unknown counter, strict-default replay and border fix'
    Sources     = @('window_present_counter.cpp')
    IncludeDirs = @('.', '{repo}/OptiScaler')
}
