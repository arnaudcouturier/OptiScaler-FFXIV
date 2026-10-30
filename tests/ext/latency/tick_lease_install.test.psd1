@{
    Class       = 'cpu'
    Note        = 'The tick lease''s product order on a stand-in image: pointers to unmapped memory refused before they are read, the layout check over resolved offsets only, the pin before the write, the compare-exchange stand-in calling the observer from inside the exchange (it must run the original), the shutdown pass-through, the restore, and the key-off present path (source pins plus a retry-plan stress)'
    Sources     = @('tick_lease_install.cpp', '{repo}/OptiScaler/ext/latency/TickLease.cpp')
    IncludeDirs = @('lease_stubs', '{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
