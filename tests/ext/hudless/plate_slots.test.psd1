@{
    Class       = 'cpu'
    Note        = 'The three plate slots without a GPU: reservation excludes drawable/staged/reserved slots, the free rule checks pins and every reader fence, a stale reservation is reclaimed after 500 ms, pins belong to a generation, the reader table refuses overflow'
    Sources     = @('plate_slots.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
