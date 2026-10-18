@{
    Class       = 'cpu'
    Note        = 'CharaView preview NR hand-off on a two-queue timeline model (D3D11 queue, D3D12 queue, fences with last-written-value semantics): the production RunFrame/RunHandoff/ConsumerWait drive it under 403 schedules per scenario, late and default variants, several streams; no hang, no fence value lowered, no wait satisfied early, every data dependency holds; the first version''s single shared fence is the negative control'
    Sources     = @('chara_nr_timeline.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
