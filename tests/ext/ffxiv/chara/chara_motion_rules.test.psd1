@{
    Class       = 'cpu'
    Note        = 'CharaView signed-motion replay rules: when a G-buffer draw is replayed (tagged or exact shape), every skip condition (UAV, stream-out, open query, deferred, multisample, transparent), the wider camera-block size of the opt-in replay shape, the depth state of the replay, the depth-state cache and the open-query set'
    Sources     = @('chara_motion_rules.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Libs        = @('d3d11.lib')
    Defines     = @('NOMINMAX')
}
