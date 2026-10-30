@{
    Class       = 'warp'
    Note        = 'Native nameplate engine protocol on WARP: three slots reserved before any hide, one record per present, the pass pins what it draws, steady state, warm-up, skip, cutscene, release, device change, key toggles and the drain, multiplier 1/2, pause, resize, refused reservation, produce failure, thread affinity, late and early outputs, the seam routing, the key-off hand-back on a lagging queue, a 300-present run, mover shifts, a shutdown release, the hidden-skip breaker, a late pass, a two-thread soak with a forced interleaving, a failed pass build, a failed device and a release under a running pass'
    Prepare     = 'prepare_ui_shaders.ps1'
    Sources     = @('plate_engine_protocol.cpp', '{repo}/OptiScaler/ext/hudless/UiPlates.cpp', '{repo}/OptiScaler/ext/hudless/SharedSlot.cpp')
    IncludeDirs = @('plate_stubs', 'ui_stubs', '../gpu/stubs', '{out}', '{repo}/OptiScaler')
    Libs        = @('d3d11.lib', 'd3d12.lib', 'dxgi.lib', 'ole32.lib')
    Defines     = @('PLATES_CHECKED_MUTEX', 'PLATES_SEAM_ROUTING', 'NOMINMAX', 'PLATES_TEST_CLOCK', 'PLATES_NOW=plate_test_clock::Now')
    Std         = 'c++20'
}
