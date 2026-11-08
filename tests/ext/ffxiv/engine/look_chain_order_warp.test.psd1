@{
    Class       = 'warp'
    Note        = 'INT1: the tone-map chain''s order as built, driven at run time on D3D11 WARP: the real dispatchers (ChainHooks.h), the real night grade (answers, grades, owns the game''s draw), the real light bounce''s off and declining exits, and stand-ins that follow the chain''s contract for what needs the engine''s captures (the light bounce''s answer, the atmosphere, depth of field); every combination of light bounce (off, declining, answering), night grade (key off, day weight 0, night), atmosphere (not linked, off, on) and depth of field (off, on) through three entries (the chained texture hook, the march''s owner hook, the seam): each enabled stage is handed the previous enabled stage''s texture (the pixels are checked), a disabled stage is a bit-exact bypass, the game''s tone-map draw happens exactly once, the owner-return rule holds, the last answering stage''s texture is at t0 for the draw and the game''s view comes back, and a second call in the same frame answers nothing'
    Prepare     = 'prepare_look_chain.ps1'
    Sources     = @('look_chain_order_warp.cpp',
                    '{repo}/OptiScaler/ext/ffxiv/engine/GiDraw.cpp',
                    '{repo}/OptiScaler/ext/ffxiv/engine/GiPass.cpp',
                    '{repo}/OptiScaler/ext/ffxiv/enginelight/EngineLightDraw.cpp',
                    '{repo}/OptiScaler/ext/ffxiv/enginelight/EngineLightPass.cpp',
                    '{repo}/OptiScaler/ext/ffxiv/enginelight/EngineLightLamps.cpp',
                    '{repo}/OptiScaler/ext/ffxiv/engine/ShaftsSun.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler', '{out}')
    Libs        = @('d3d11.lib', 'dxgi.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
