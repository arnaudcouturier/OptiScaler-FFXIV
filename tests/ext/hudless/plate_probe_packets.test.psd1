@{
    Class       = 'cpu'
    Note        = 'Plate probe packet association (ext/hudless/PlateProbePackets.h): logged-frame gate, packet header and range decoding, index list against packets, vertex run and used flags, vertex box (min/max, stride, position offset, bad numbers), ordinals, association map (age, changed command, capacity, ordinal lookup), draw match, draw command fields, overlay mapping and slot guard'
    Sources     = @('plate_probe_packets.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
