@{
    Class       = 'warp'
    Note        = 'Engine light controls at the composite and the tone map, driven through the module''s own entries on D3D11 WARP with the neighbours stubbed (depth of field behind the chain hooks as a recorder, the night curve behind the shadow module''s hook, the cb family''s receiver behind the contract''s PublishCbFamily, an injected wall clock, a real composite pixel shader whose reflection puts g_CommonParameter at b3 and carries the module''s stamp): master off arms nothing and still hands the stage after the incoming scene, a lamps-only configuration is armed and FED (the retired grade''s feed was tied to its own request) and its weight and lamp factor follow the night with no exposure reading and are exactly 0 and 1 by day, the A/B makes the factor exactly 1 and reaches the receiver, the forced weight reaches 1 with no reading and no feed, the auto hold waits (weight 0, no pass) for the engine''s exposure to settle, latches it, ramps, never follows the rise the controls cause, answers stored x s_hold / s_now with THIS frame''s s read on the GPU (a jump of the exposure is followed in the same frame), hands the stage after that answer, answers once a frame, falls back to gain 1 on a buffer that fails the fingerprint, owns the game''s draw exactly once with the stage''s texture at t0 and puts the game''s view back (a throw is not retried), the manual factor eases with the weight, and sixty-four shape declines stand the exposure stage down while the module goes on'
    Prepare     = 'prepare_engine_light.ps1'
    Sources     = @('engine_light_chain_warp.cpp',
                    '{repo}/OptiScaler/ext/ffxiv/enginelight/EngineLightDraw.cpp',
                    '{repo}/OptiScaler/ext/ffxiv/enginelight/EngineLightPass.cpp',
                    '{repo}/OptiScaler/ext/ffxiv/enginelight/EngineLightLamps.cpp',
                    '{repo}/OptiScaler/ext/ffxiv/engine/ShaftsSun.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler', '{out}')
    Libs        = @('d3d11.lib', 'dxgi.lib', 'd3dcompiler.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
