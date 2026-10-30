@{
    Class       = 'cpu'
    Note        = 'The tick probe''s product taps with a fake microsecond clock and a recording sink: nothing at all with the key off, arming only after the lease is installed (a tick in flight then is not recorded), the counter value on every event, the note de-duplication, the 10 s line through the log sink for a present after its tick and for a present nested inside it on the same thread, and source pins for the one-load key gate and the config-load mirror'
    Sources     = @('tick_probe_product.cpp', '{repo}/OptiScaler/ext/latency/TickProbe.cpp')
    IncludeDirs = @('probe_stubs', '{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
