@{
    Class       = 'cpu'
    Note        = 'The tick lease with telemetry offsets that did not resolve: an unresolved offset is neither compared nor read, the lease still installs, the line says counter=n/a and the observer records a tick without a frame counter; a contradiction between the two offsets that did resolve refuses'
    Sources     = @('tick_lease_offsets.cpp', '{repo}/OptiScaler/ext/latency/TickLease.cpp')
    IncludeDirs = @('lease_stubs', '{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
