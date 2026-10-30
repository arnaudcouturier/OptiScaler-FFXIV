@{
    Class       = 'cpu'
    Note        = 'FG recorder queue cap and dropped-records record: bounded at 4096, overflow counted in one explicit record, and a live burst accounts every event as kept or dropped (a raised cap or silent loss fails)'
    Sources     = @('fgrec_queue.cpp')
    IncludeDirs = @('stubs', '.', '{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
