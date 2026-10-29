@{
    Class       = 'cpu'
    Note        = 'Plate probe event ring (ext/hudless/PlateProbeRing.h): a full ring drops and counts and never blocks, four producers racing one consumer lose and duplicate nothing and keep their own order'
    Sources     = @('plate_probe_ring.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
