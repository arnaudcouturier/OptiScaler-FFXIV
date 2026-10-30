@{
    Class       = 'cpu'
    Note        = 'CharaView slot and recent-list state machine with an injected clock: 250 ms recent/live/reset windows, 2000 ms idle release gated on the GPU query, 16 slots, failure sticks, rebuild on size or depth convention change, and the least-recently-used eviction of an idle slot whose GPU work is finished'
    Sources     = @('chara_slots.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
